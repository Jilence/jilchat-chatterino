// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/ffzap/FfzApBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

#include <mutex>

namespace chatterino {

namespace {

using namespace Qt::Literals;

QString badgeNameForTier(int tier)
{
    if (tier > 0)
    {
        return u"FFZ:AP Supporter Tier %1"_s.arg(tier);
    }

    return u"FFZ:AP Supporter"_s;
}

EmotePtr makeFfzApBadge(const QString &userId, int tier)
{
    if (userId.isEmpty())
    {
        return nullptr;
    }

    const auto tooltip = badgeNameForTier(tier);
    auto emote = Emote{
        .name = EmoteName{u"ffzap:supporter"_s},
        .images = ImageSet{Image::fromAutoscaledUrl(
            Url{u"https://api.ffzap.com/v1/user/badge/%1/2"_s.arg(userId)},
            18)},
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{userId},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

}  // namespace

FfzApBadges::FfzApBadges()
{
    this->loadFfzApBadges();
}

void FfzApBadges::loadFfzApBadges()
{
    static QUrl url("https://api.ffzap.com/v1/supporters");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            const auto jsonRoot = result.parseJsonValue();
            if (!jsonRoot.isArray())
            {
                return Failure;
            }

            this->applySupportersJson(jsonRoot.toArray());
            return Success;
        })
        .execute();
}

std::optional<EmotePtr> FfzApBadges::getBadge(const UserId &id) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->badgeMap_.find(id.string);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void FfzApBadges::applySupportersJson(const QJsonArray &jsonRoot)
{
    std::unordered_map<QString, EmotePtr> badgeMap;
    badgeMap.reserve(static_cast<size_t>(jsonRoot.size()));

    for (const auto &jsonSupporterValue : jsonRoot)
    {
        const auto jsonSupporter = jsonSupporterValue.toObject();
        const auto userId = jsonSupporter.value("id").toString();
        auto emote = makeFfzApBadge(userId, jsonSupporter.value("tier").toInt());
        if (!emote)
        {
            continue;
        }

        badgeMap.emplace(userId, std::move(emote));
    }

    std::unique_lock lock(this->mutex_);
    this->badgeMap_ = std::move(badgeMap);
}

}  // namespace chatterino
