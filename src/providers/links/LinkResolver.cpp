// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkResolver.hpp"

#include "common/Env.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/TikTokLinkResolver.hpp"
#include "singletons/Settings.hpp"

#include <QCache>
#include <QStringBuilder>

#include <chrono>
#include <optional>

namespace chatterino {

namespace {

using Clock = std::chrono::steady_clock;

constexpr qsizetype TIKTOK_CACHE_LIMIT = 32;
constexpr auto TIKTOK_CACHE_LIFETIME = std::chrono::minutes(30);
constexpr int TIKTOK_TIMEOUT_MS = 8000;

struct CachedTikTokPreview {
    tiktok::Preview preview;
    Clock::time_point expiresAt;
};

QCache<QString, CachedTikTokPreview> &tiktokCache()
{
    static QCache<QString, CachedTikTokPreview> cache(TIKTOK_CACHE_LIMIT);
    return cache;
}

std::optional<CachedTikTokPreview> cachedTikTokPreview(const QString &key)
{
    auto &cache = tiktokCache();
    auto *preview = cache.object(key);
    if (!preview)
    {
        return std::nullopt;
    }
    if (preview->expiresAt <= Clock::now())
    {
        cache.remove(key);
        return std::nullopt;
    }
    return *preview;
}

void applyTikTokPreview(LinkInfo *info, const CachedTikTokPreview &cached)
{
    info->setTooltip(tiktok::makeTooltip(info->originalUrl(), cached.preview));
    if (!cached.preview.thumbnailUrl.isEmpty())
    {
        info->setThumbnail(Image::fromUrl({cached.preview.thumbnailUrl}));
    }
    info->setState(LinkInfo::State::Resolved);
}

void failTikTokPreview(LinkInfo *info, const QString &reason)
{
    const auto logUrl = QUrl(info->originalUrl())
                            .adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery |
                                      QUrl::RemoveFragment);
    qCDebug(chatterinoHTTP)
        << "TikTok link preview failed for" << logUrl << reason;
    info->setTooltip(tiktok::makeUnavailableTooltip(info->originalUrl()));
    info->setState(LinkInfo::State::Errored);
}

/// TikTok videos and profiles are described by TikTok's own oEmbed, which
/// the generic link resolver can't read. `aliasCacheKey` is the short link
/// that led here, cached as well.
void requestTikTokOEmbed(LinkInfo *info, const tiktok::LinkTarget &target,
                         QString aliasCacheKey = {})
{
    NetworkRequest(tiktok::makeOEmbedUrl(target.url))
        .caller(info)
        .timeout(TIKTOK_TIMEOUT_MS)
        .followRedirects(false)
        .header("Accept", "application/json")
        .onSuccess([info, target, aliasCacheKey = std::move(aliasCacheKey)](
                       const NetworkResult &result) {
            const auto preview = tiktok::parseOEmbed(result.parseJson());
            if (!preview)
            {
                failTikTokPreview(info, QStringLiteral("invalid response"));
                return;
            }

            const CachedTikTokPreview cached{
                .preview = *preview,
                .expiresAt = Clock::now() + TIKTOK_CACHE_LIFETIME,
            };
            tiktokCache().insert(target.cacheKey,
                                 new CachedTikTokPreview(cached));
            if (!aliasCacheKey.isEmpty() && aliasCacheKey != target.cacheKey)
            {
                tiktokCache().insert(aliasCacheKey,
                                     new CachedTikTokPreview(cached));
            }
            applyTikTokPreview(info, cached);
        })
        .onError([info](const NetworkResult &result) {
            failTikTokPreview(info, result.formatError());
        })
        .execute();
}

/// Follows a short vm.tiktok.com link to the video or profile it stands for
void resolveShortTikTokLink(LinkInfo *info,
                            const tiktok::LinkTarget &shortTarget)
{
    NetworkRequest(shortTarget.url)
        .caller(info)
        .timeout(TIKTOK_TIMEOUT_MS)
        .followRedirects(true)
        .onSuccess([info, shortTarget](const NetworkResult &result) {
            const auto target = tiktok::parseLink(result.url());
            if (!target || target->kind == tiktok::LinkKind::Short)
            {
                failTikTokPreview(info, QStringLiteral("invalid redirect"));
                return;
            }

            if (const auto cached = cachedTikTokPreview(target->cacheKey))
            {
                tiktokCache().insert(shortTarget.cacheKey,
                                     new CachedTikTokPreview(*cached));
                applyTikTokPreview(info, *cached);
                return;
            }

            requestTikTokOEmbed(info, *target, shortTarget.cacheKey);
        })
        .onError([info](const NetworkResult &result) {
            failTikTokPreview(info, result.formatError());
        })
        .execute();
}

std::optional<tiktok::LinkTarget> tiktokTarget(const QString &url)
{
    if (!url.contains(QStringLiteral("tiktok.com"), Qt::CaseInsensitive))
    {
        return std::nullopt;
    }
    return tiktok::parseLink(QUrl::fromUserInput(url));
}

}  // namespace

void LinkResolver::resolve(LinkInfo *info)
{
    using State = LinkInfo::State;

    assert(info);

    if (info->state() != State::Created)
    {
        return;
    }

    if (!getSettings()->linkInfoTooltip)
    {
        return;
    }

    info->setTooltip("Loading...");
    info->setState(State::Loading);

    if (const auto target = tiktokTarget(info->originalUrl()))
    {
        if (const auto cached = cachedTikTokPreview(target->cacheKey))
        {
            applyTikTokPreview(info, *cached);
            return;
        }
        if (target->kind == tiktok::LinkKind::Short)
        {
            resolveShortTikTokLink(info, *target);
            return;
        }
        requestTikTokOEmbed(info, *target);
        return;
    }

    NetworkRequest(Env::get().linkResolverUrl.arg(QString::fromUtf8(
                       QUrl::toPercentEncoding(info->originalUrl(), {}, "/:"))))
        .caller(info)
        .timeout(30000)
        .onSuccess([info](const NetworkResult &result) {
            const auto root = result.parseJson();
            QString response;
            QString url;
            ImagePtr thumbnail = nullptr;
            if (root["status"].toInt() == 200)
            {
                response = root["tooltip"].toString();

                if (root.contains("thumbnail"))
                {
                    info->setThumbnail(
                        Image::fromUrl({root["thumbnail"].toString()}));
                }
                if (getSettings()->unshortLinks && root.contains("link"))
                {
                    info->setResolvedUrl(root["link"].toString());
                }
            }
            else
            {
                response = root["message"].toString();
            }

            info->setTooltip(QUrl::fromPercentEncoding(response.toUtf8()));
            info->setState(State::Resolved);
        })
        .onError([info](const auto &result) {
            info->setTooltip(u"No link info found (" % result.formatError() %
                             u')');
            info->setState(State::Errored);
        })
        .execute();
}

}  // namespace chatterino
