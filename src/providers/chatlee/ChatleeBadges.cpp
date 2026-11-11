// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/chatlee/ChatleeBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <chrono>

namespace chatterino {

namespace {

using namespace std::chrono_literals;

constexpr auto API = "https://api.chatlee.lol/api/chatlee/badges";
constexpr auto IMAGE_HOST = "api.chatlee.lol";
constexpr qsizetype MAX_PAYLOAD_SIZE = 8 * 1024 * 1024;
/// What Chatlee's documentation asks for
constexpr auto CATALOG_INTERVAL = 30min;
constexpr auto WORN_INTERVAL = 60s;

bool validImageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    return value.size() <= 2048 && url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           url.host() == QLatin1String(IMAGE_HOST) &&
           url.userInfo().isEmpty() && url.port(-1) == -1 &&
           !url.hasFragment() &&
           url.path().startsWith(QStringLiteral("/api/files/"));
}

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

QJsonObject parseObject(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return {};
    }
    const auto document = QJsonDocument::fromJson(payload);
    return document.isObject() ? document.object() : QJsonObject{};
}

}  // namespace

QByteArray ChatleeBadges::userAgent()
{
    return QStringLiteral("JilChatChatterino/%1 (+https://github.com/Jilence/"
                          "jilchat-chatterino)")
        .arg(Version::instance().version())
        .toUtf8();
}

EmotePtr ChatleeBadges::makeBadge(const QString &id, const QString &title,
                                  const QString &imageUrl)
{
    if (id.isEmpty() || id.size() > 64 || title.isEmpty() ||
        title.size() > 256 || !validImageUrl(imageUrl))
    {
        return nullptr;
    }

    // One image of 96 pixels for all three sizes: each is read in at its
    // size (18, 36 and the full 96 pixels) and shown at 18 in chat. The hover
    // shows the biggest at its size, so it's as big as other 96-pixel badges
    // (like Homies')
    const Url url{imageUrl};
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("chatlee:") + id},
        .images =
            ImageSet{
                Image::fromUrl(url, 1.0, QSize(18, 18)),
                Image::fromUrl(url, 0.5, QSize(36, 36)),
                Image::fromUrl(url, 18.0 / 96.0, QSize(96, 96)),
            },
        .tooltip = Tooltip{title.toHtmlEscaped()},
        .homePage = Url{QStringLiteral("https://chatlee.lol")},
        .id = EmoteId{imageUrl},
    });
}

ChatleeBadges::ChatleeBadges()
{
    this->catalogTimer_.setInterval(CATALOG_INTERVAL);
    QObject::connect(&this->catalogTimer_, &QTimer::timeout, this, [this] {
        if (getSettings()->showBadgesChatlee)
        {
            this->loadCatalog({});
        }
    });
    this->catalogTimer_.start();

    this->wornTimer_.setInterval(WORN_INTERVAL);
    QObject::connect(&this->wornTimer_, &QTimer::timeout, this, [this] {
        if (getSettings()->showBadgesChatlee && this->wornVersion_ >= 0)
        {
            this->loadWorn(false, {});
        }
    });
    this->wornTimer_.start();

    // Nothing is requested from Chatlee while its badges are turned off
    getSettings()->showBadgesChatlee.connect(
        [this](bool enabled, const auto &) {
            if (enabled)
            {
                QTimer::singleShot(4000, this, [this] {
                    this->load();
                });
            }
        },
        this->signalHolder_);
}

std::optional<EmotePtr> ChatleeBadges::getBadge(const QString &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto worn = this->worn_.find(userID);
    if (worn == this->worn_.end())
    {
        return std::nullopt;
    }
    const auto badge = this->catalog_.find(worn->second);
    if (badge == this->catalog_.end())
    {
        return std::nullopt;
    }
    return badge->second;
}

void ChatleeBadges::load(const std::function<void(bool)> &onDone)
{
    if (this->loading_)
    {
        if (onDone)
        {
            onDone(true);
        }
        return;
    }
    this->loading_ = true;
    // The catalog first, so the badges people wear are known
    this->catalogEtag_.clear();
    this->loadCatalog([this, onDone](bool catalogLoaded) {
        this->loadWorn(true, [this, onDone, catalogLoaded](bool wornLoaded) {
            this->loading_ = false;
            if (onDone)
            {
                onDone(catalogLoaded && wornLoaded);
            }
        });
    });
}

bool ChatleeBadges::applyCatalog(const QByteArray &payload)
{
    const auto root = parseObject(payload);
    if (!root.value("badges").isArray())
    {
        return false;
    }

    std::unordered_map<QString, EmotePtr> catalog;
    const auto badges = root.value("badges").toArray();
    if (badges.size() > 1024)
    {
        return false;
    }
    for (const auto &value : badges)
    {
        const auto object = value.toObject();
        const auto id = object.value("id").toString();
        const auto slug = object.value("slug").toString();
        auto badge = makeBadge(slug.isEmpty() ? id : slug,
                               object.value("title").toString(),
                               object.value("image_url").toString());
        if (badge == nullptr)
        {
            continue;
        }
        // People's badges are named by either
        if (!id.isEmpty())
        {
            catalog.emplace(id, badge);
        }
        if (!slug.isEmpty())
        {
            catalog.emplace(slug, badge);
        }
    }

    std::unique_lock lock(this->mutex_);
    this->catalog_ = std::move(catalog);
    return true;
}

bool ChatleeBadges::applyWorn(const QByteArray &payload)
{
    const auto root = parseObject(payload);
    if (!root.value("changes").isArray() || !root.value("version").isDouble())
    {
        return false;
    }
    const auto changes = root.value("changes").toArray();
    if (changes.size() > 500000)
    {
        return false;
    }

    std::unique_lock lock(this->mutex_);
    if (root.value("full").toBool())
    {
        this->worn_.clear();
    }
    for (const auto &value : changes)
    {
        const auto change = value.toObject();
        const auto user = change.value("user_id").toString();
        if (!validUserId(user))
        {
            continue;
        }
        const auto badge = change.value("badge").toString();
        // An empty badge: wears none
        if (badge.isEmpty())
        {
            this->worn_.erase(user);
        }
        else if (badge.size() <= 64)
        {
            this->worn_[user] = badge;
        }
    }
    this->wornVersion_ = qint64(root.value("version").toDouble());
    return true;
}

void ChatleeBadges::loadCatalog(const std::function<void(bool)> &onDone)
{
    auto request =
        NetworkRequest(QString::fromLatin1(API) + QStringLiteral("/v2/catalog"))
            .header("Accept", "application/json")
            .header("User-Agent", userAgent())
            .timeout(10000)
            .caller(this);
    if (!this->catalogEtag_.isEmpty())
    {
        request =
            std::move(request).header("If-None-Match", this->catalogEtag_);
    }
    std::move(request)
        .onSuccess([this, onDone](const NetworkResult &result) {
            // 304: unchanged
            if (result.status() != 304)
            {
                if (!this->applyCatalog(result.getData()))
                {
                    qCWarning(chatterinoApp)
                        << "[Chatlee] Ignoring an invalid badge catalog.";
                    if (onDone)
                    {
                        onDone(false);
                    }
                    return;
                }
                this->catalogEtag_ = result.etag();
                WindowManager::notifyBadgesUpdated();
            }
            if (onDone)
            {
                onDone(true);
            }
        })
        .onError([onDone](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Chatlee] Catalog request failed:" << result.formatError();
            if (onDone)
            {
                onDone(false);
            }
        })
        .execute();
}

void ChatleeBadges::loadWorn(bool full, const std::function<void(bool)> &onDone)
{
    QString url = QString::fromLatin1(API) + QStringLiteral("/v2/worn");
    if (!full && this->wornVersion_ >= 0)
    {
        url += QStringLiteral("?since=") + QString::number(this->wornVersion_);
    }
    NetworkRequest(url)
        .header("Accept", "application/json")
        .header("User-Agent", userAgent())
        .timeout(15000)
        .caller(this)
        .onSuccess([this, onDone](const NetworkResult &result) {
            const auto before = this->wornVersion_;
            if (!this->applyWorn(result.getData()))
            {
                qCWarning(chatterinoApp)
                    << "[Chatlee] Ignoring an invalid list of worn badges.";
                if (onDone)
                {
                    onDone(false);
                }
                return;
            }
            if (this->wornVersion_ != before)
            {
                WindowManager::notifyBadgesUpdated();
            }
            if (onDone)
            {
                onDone(true);
            }
        })
        .onError([onDone](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Chatlee] Request failed:" << result.formatError();
            if (onDone)
            {
                onDone(false);
            }
        })
        .execute();
}

}  // namespace chatterino
