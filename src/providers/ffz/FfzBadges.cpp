// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/ffz/FfzBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/ffz/FfzUtil.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QThread>
#include <QUrl>

#include <functional>

namespace chatterino {

std::vector<FfzBadges::Badge> FfzBadges::getUserBadges(const UserId &id)
{
    std::vector<Badge> badges;

    std::shared_lock lock(this->mutex_);

    auto it = this->userBadges.find(id.string);
    if (it != this->userBadges.end())
    {
        for (const auto &badgeID : it->second)
        {
            if (auto badge = this->getBadge(badgeID); badge)
            {
                badges.emplace_back(*badge);
            }
        }
    }

    return badges;
}

std::optional<FfzBadges::Badge> FfzBadges::getBadge(const int badgeID) const
{
    this->tgBadges.guard();
    auto it = this->badges.find(badgeID);
    if (it != this->badges.end())
    {
        return it->second;
    }

    return std::nullopt;
}

void FfzBadges::load(const std::function<void(bool)> &onDone)
{
    static QUrl url("https://api.frankerfacez.com/v1/badges/ids");

    NetworkRequest(url)
        .onSuccess([this, onDone](const auto &result) {
            std::unique_lock lock(this->mutex_);

            auto jsonRoot = result.parseJson();
            this->tgBadges.guard();

            const auto jsonBadges = jsonRoot.value("badges").toArray();
            if (jsonBadges.isEmpty())
            {
                // an answer without badges: keep what was loaded before
                lock.unlock();
                if (onDone)
                {
                    onDone(false);
                }
                return;
            }

            // loading again replaces the badges and who has them
            this->badges.clear();
            this->userBadges.clear();

            for (const auto &jsonBadgeValue : jsonBadges)
            {
                auto jsonBadge = jsonBadgeValue.toObject();
                auto jsonUrls = jsonBadge.value("urls").toObject();
                QSize baseSize(jsonBadge["width"].toInt(18),
                               jsonBadge["height"].toInt(18));

                auto emote = Emote{
                    .name =
                        EmoteName{
                            u"frankerfacez:" %
                                jsonBadge.value("name").toString(),
                        },
                    .images =
                        ImageSet{
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("1").toString()),
                                1.0, baseSize),
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("2").toString()),
                                0.5, baseSize * 2),
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("4").toString()),
                                0.25, baseSize * 4)},
                    .tooltip = Tooltip{jsonBadge.value("title").toString()},
                    .homePage = Url{},
                };

                int badgeID = jsonBadge.value("id").toInt();

                this->badges[badgeID] = Badge{
                    .emote = std::make_shared<const Emote>(std::move(emote)),
                    .color = QColor(jsonBadge.value("color").toString()),
                };

                auto badgeIDString = QString::number(badgeID);
                for (const auto &user : jsonRoot.value("users")
                                            .toObject()
                                            .value(badgeIDString)
                                            .toArray())
                {
                    auto userIDString = QString::number(user.toInt());

                    auto [userBadges, created] = this->userBadges.emplace(
                        std::move(userIDString),
                        QVarLengthArray<int, 2>{badgeID});
                    if (!created)
                    {
                        // User already had a badge assigned
                        if (!userBadges->second.contains(badgeID))
                        {
                            userBadges->second.emplace_back(badgeID);
                        }
                    }
                }
            }

            WindowManager::notifyBadgesUpdated();

            // not while holding the lock: the callback may write to a channel
            lock.unlock();
            if (onDone)
            {
                onDone(true);
            }
        })
        .onError([onDone](const auto & /*result*/) {
            if (onDone)
            {
                onDone(false);
            }
        })
        .execute();
}

void FfzBadges::registerBadge(int badgeID, Badge badge)
{
    assert(getApp()->isTest());

    std::unique_lock lock(this->mutex_);

    this->badges.emplace(badgeID, std::move(badge));
}

void FfzBadges::assignBadgeToUser(const UserId &userID, int badgeID)
{
    assert(getApp()->isTest());

    std::unique_lock lock(this->mutex_);

    auto it = this->userBadges.find(userID.string);
    if (it != this->userBadges.end())
    {
        if (!it->second.contains(badgeID))
        {
            it->second.emplace_back(badgeID);
        }
    }
    else
    {
        this->userBadges.emplace(userID.string,
                                 QVarLengthArray<int, 2>{badgeID});
    }
}

}  // namespace chatterino
