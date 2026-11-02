// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/streamnook/StreamNookBadges.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringBuilder>

#include <chrono>

namespace chatterino {

using namespace chatterino::literals;
using namespace std::chrono_literals;

namespace {

constexpr QSize BADGE_BASE_SIZE(18, 18);

/// The file is cached for five minutes at StreamNook's edge
constexpr auto REFRESH_INTERVAL = 5min;

const QString BADGES_URL =
    u"https://streamnook.app/api/v1/cosmetics/badges/users"_s;

ImageSet createBadgeImages(const QString &url)
{
    // One large file for every size; three tiers keep the hover preview
    // sharp
    return ImageSet{
        Image::fromUrl(Url{url}, 1.0, BADGE_BASE_SIZE),
        Image::fromUrl(Url{url}, 0.5, BADGE_BASE_SIZE * 2),
        Image::fromUrl(Url{url}, 0.25, BADGE_BASE_SIZE * 4),
    };
}

}  // namespace

StreamNookBadges::StreamNookBadges()
{
    this->refreshTimer_.setInterval(REFRESH_INTERVAL);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        if (getSettings()->showBadgesStreamNook)
        {
            this->load();
        }
    });
    this->refreshTimer_.start();

    // Nothing is requested while the badges are turned off
    getSettings()->showBadgesStreamNook.connect(
        [this](bool enabled, auto) {
            if (enabled)
            {
                QTimer::singleShot(3500, this, [this] {
                    this->load();
                });
            }
        },
        this->signalHolder_);
}

std::optional<EmotePtr> StreamNookBadges::getBadge(const QString &userID) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->badgeMap_.find(userID);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void StreamNookBadges::load(const std::function<void(bool)> &onDone)
{
    NetworkRequest(BADGES_URL)
        .concurrent()
        .timeout(10000)
        .onSuccess([this, onDone](const NetworkResult &result) {
            const auto root = result.parseJson();

            std::unordered_map<QString, EmotePtr> bySlug;
            for (const auto &value : root[u"badges"_s].toArray())
            {
                const auto badge = value.toObject();
                const auto slug = badge[u"slug"_s].toString();
                const auto url = badge[u"image_url"_s].toString();
                if (slug.isEmpty() || url.isEmpty())
                {
                    continue;
                }

                const auto tooltip =
                    badge[u"name"_s].toString().toHtmlEscaped();

                bySlug.emplace(slug,
                               std::make_shared<const Emote>(Emote{
                                   .name = EmoteName{u"streamnook:" % slug},
                                   .images = createBadgeImages(url),
                                   .tooltip = Tooltip{tooltip},
                                   .homePage = Url{u"https://streamnook.app"_s},
                                   .id = EmoteId{slug},
                               }));
            }

            // Members hold several badges but wear one: `applied`
            std::unordered_map<QString, EmotePtr> map;
            const auto users = root[u"users"_s].toObject();
            for (auto it = users.begin(); it != users.end(); ++it)
            {
                const auto applied =
                    it.value().toObject()[u"applied"_s].toString();
                const auto badge = bySlug.find(applied);
                if (badge != bySlug.end())
                {
                    map.emplace(it.key(), badge->second);
                }
            }

            const auto count = map.size();
            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(map);
            }
            WindowManager::notifyBadgesUpdated();

            qCDebug(chatterinoApp)
                << "[StreamNook] Loaded badges for" << count << "users";
            if (onDone)
            {
                onDone(true);
            }
        })
        .onError([onDone](const NetworkResult &result) {
            qCWarning(chatterinoApp) << "[StreamNook] Failed to load badges:"
                                     << result.formatError();
            if (onDone)
            {
                onDone(false);
            }
        })
        .execute();
}

}  // namespace chatterino
