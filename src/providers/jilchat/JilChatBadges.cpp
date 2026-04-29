// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jilchat/JilChatBadges.hpp"

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

EmotePtr makeJilChatBadge(const QJsonObject &badgeJson)
{
    const auto id = badgeJson.value("id").toString();
    const auto slug = badgeJson.value("slug").toString();
    const auto name = badgeJson.value("name").toString();
    const auto imageUrl = badgeJson.value("image_url").toString();

    if (id.isEmpty() || name.isEmpty() || imageUrl.isEmpty())
    {
        return nullptr;
    }

    auto emote = Emote{
        .name = EmoteName{u"jilchat:" % (slug.isEmpty() ? name : slug)},
        .images = ImageSet{Image::fromAutoscaledUrl(Url{imageUrl}, 18)},
        .tooltip = Tooltip{name},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

}  // namespace

JilChatBadges::JilChatBadges()
{
    this->loadJilChatBadges();
}

void JilChatBadges::loadJilChatBadges()
{
    static QUrl url("https://api.jil.chat/v1/badges");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            const auto jsonRoot = result.parseJsonValue();
            if (!jsonRoot.isArray())
            {
                return Failure;
            }

            this->applyBadgeJson(jsonRoot.toArray());
            return Success;
        })
        .execute();
}

std::vector<EmotePtr> JilChatBadges::getBadges(const UserId &id) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->badgeMap_.find(id.string);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return {};
}

void JilChatBadges::applyBadgeJson(const QJsonArray &jsonRoot)
{
    std::unordered_map<QString, std::vector<EmotePtr>> badgeMap;

    for (const auto &jsonBadgeValue : jsonRoot)
    {
        const auto jsonBadge = jsonBadgeValue.toObject();
        auto emote = makeJilChatBadge(jsonBadge);
        if (!emote)
        {
            continue;
        }

        const auto users = jsonBadge.value("users").toArray();
        for (const auto &jsonUserValue : users)
        {
            const auto user = jsonUserValue.toObject();
            const auto userId = user.value("twitch_id").toString();
            if (userId.isEmpty())
            {
                continue;
            }

            badgeMap[userId].push_back(emote);
        }
    }

    std::unique_lock lock(this->mutex_);
    this->badgeMap_ = std::move(badgeMap);
}

}  // namespace chatterino
