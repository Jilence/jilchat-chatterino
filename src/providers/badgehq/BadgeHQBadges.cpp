// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/badgehq/BadgeHQBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/Settings.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSize>
#include <QUrl>

#include <algorithm>
#include <mutex>

namespace chatterino {
namespace {

constexpr auto ORIGIN = "https://www.badgehq.xyz";
constexpr auto BADGES_URL = "https://www.badgehq.xyz/api/v1/badges";
constexpr qsizetype MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
constexpr auto REFRESH_INTERVAL = 30 * 60 * 1000;
constexpr QSize BADGE_BASE_SIZE{18, 18};

/// Badge images are only ever loaded from BadgeHQ itself: the path of the
/// listed image is kept and requested from the BadgeHQ origin.
QString imageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    if (value.isEmpty() || value.size() > 2048 || !url.isValid() ||
        !url.path().startsWith(QStringLiteral("/badges/")))
    {
        return {};
    }
    auto result = QString::fromLatin1(ORIGIN) + url.path(QUrl::FullyEncoded);
    if (url.hasQuery())
    {
        result += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    }
    return result;
}

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

ImagePtr badgeImage(const QString &value, qreal scale, QSize expectedSize)
{
    const auto url = imageUrl(value);
    if (url.isEmpty())
    {
        return Image::getEmpty();
    }
    return Image::fromUrl(Url{url}, scale, expectedSize);
}

std::shared_ptr<const Emote> makeBadge(const QJsonObject &object)
{
    const auto id = object.value("id").toString();
    const auto tooltip = object.value("tooltip").toString();
    const auto images = object.value("images").toObject();
    const auto image1 = images.value("1x").toString();
    if (id.isEmpty() || id.size() > 64 || tooltip.isEmpty() ||
        tooltip.size() > 256 || imageUrl(image1).isEmpty())
    {
        return nullptr;
    }

    // BadgeHQ serves 18, 36 and 72 pixel images as 1x, 2x and 3x.
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("badgehq:") + id},
        .images =
            ImageSet{
                badgeImage(image1, 1.0, BADGE_BASE_SIZE),
                badgeImage(images.value("2x").toString(), 0.5,
                           BADGE_BASE_SIZE * 2),
                badgeImage(images.value("3x").toString(), 0.25,
                           BADGE_BASE_SIZE * 4),
            },
        .tooltip = Tooltip{tooltip.toHtmlEscaped()},
        .homePage = Url{ORIGIN},
        .id = EmoteId{id},
    });
}

}  // namespace

void BadgeHQBadges::initialize()
{
    this->refreshTimer_.setInterval(REFRESH_INTERVAL);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->requestBadges();
    });
    this->refreshTimer_.start();

    // Nothing is requested from BadgeHQ while its badges are turned off.
    // The callback runs immediately, so an already-on setting fetches once
    // startup has settled.
    getSettings()->showBadgesBadgeHQ.connect(
        [this](bool enabled, auto) {
            if (enabled)
            {
                QTimer::singleShot(3500, this, [this] {
                    this->requestBadges();
                });
            }
        },
        this->signalHolder_);
}

std::vector<std::shared_ptr<const Emote>> BadgeHQBadges::getBadges(
    const UserId &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto found = this->users_.find(userID.string);
    if (found == this->users_.end())
    {
        return {};
    }
    return found->second;
}

bool BadgeHQBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
    {
        return false;
    }
    const auto root = document.object();
    if (!root.value("badges").isArray() || !root.value("users").isObject())
    {
        return false;
    }

    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>> badges;
    for (const auto &value : root.value("badges").toArray())
    {
        if (const auto badge = makeBadge(value.toObject()))
        {
            badges.emplace(badge->id.string, badge);
        }
    }

    boost::unordered_flat_map<QString,
                              std::vector<std::shared_ptr<const Emote>>>
        users;
    size_t total = 0;
    const auto userObjects = root.value("users").toObject();
    for (auto it = userObjects.begin(); it != userObjects.end(); ++it)
    {
        const auto id = it.key();
        if (!validUserId(id))
        {
            continue;
        }
        for (const auto &value :
             it.value().toObject().value("badges").toArray())
        {
            const auto found = badges.find(value.toString());
            if (found == badges.end())
            {
                continue;
            }
            if (++total > 200000)
            {
                return false;
            }
            auto &owned = users[id];
            if (owned.size() < 4)
            {
                owned.push_back(found->second);
            }
        }
    }

    std::unique_lock lock(this->mutex_);
    this->users_ = std::move(users);
    return true;
}

void BadgeHQBadges::requestBadges()
{
    if (this->requestInFlight_ || !getSettings()->showBadgesBadgeHQ)
    {
        return;
    }
    this->requestInFlight_ = true;
    NetworkRequest(QUrl(QString::fromLatin1(BADGES_URL)))
        .header("Accept", "application/json")
        .timeout(8000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto payload = result.getData();
            if (payload == this->lastPayload_)
            {
                return;
            }
            if (!this->applyPayload(payload))
            {
                qCWarning(chatterinoApp)
                    << "[BadgeHQ] Ignoring an invalid badge list.";
                return;
            }
            this->lastPayload_ = payload;
            this->badgesUpdated.invoke();
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[BadgeHQ] Badge request failed:" << result.formatError();
        })
        .finally([this] {
            this->requestInFlight_ = false;
        })
        .execute();
}

}  // namespace chatterino
