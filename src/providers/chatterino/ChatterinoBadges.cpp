#include "providers/chatterino/ChatterinoBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chatterino {

ChatterinoBadges::ChatterinoBadges()
{
    this->loadChatterinoBadges();
}

std::optional<EmotePtr> ChatterinoBadges::getBadge(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap.find(id.string);
    if (it != this->badgeMap.end())
    {
        return this->emotes[it->second];
    }
    return std::nullopt;
}

EmotePtr ChatterinoBadges::getKickBadge(uint64_t kickID)
{
    auto it = this->kickMapping.find(kickID);
    if (it != this->kickMapping.end())
    {
        return this->emotes[it->second];
    }
    return {};
}

void ChatterinoBadges::setKickMapping(const QString &twitchID, uint64_t kickID)
{
    // Lookup by Twitch ID first. Assume that most users don't have a Chatterino badge.
    auto existing = this->badgeMap.find(twitchID);
    if (existing == this->badgeMap.end())
    {
        return;
    }
    this->kickMapping.emplace(kickID, existing->second);
}

void ChatterinoBadges::reload(std::function<void(bool)> onDone)
{
    this->loadChatterinoBadges(std::move(onDone));
}

void ChatterinoBadges::loadChatterinoBadges(std::function<void(bool)> onDone)
{
    static QUrl url("https://api.chatterino.com/badges");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this, onDone](auto result) {
            auto jsonRoot = result.parseJson();

            // built on the side and swapped in, so loading again replaces
            // the badges instead of adding to them
            std::vector<EmotePtr> newEmotes;
            std::unordered_map<QString, int> newBadgeMap;

            int index = 0;
            for (const auto &jsonBadgeValue :
                 jsonRoot.value("badges").toArray())
            {
                auto jsonBadge = jsonBadgeValue.toObject();

                constexpr QSize baseSize(18, 18);
                auto tooltip = jsonBadge.value("tooltip").toString();
                auto emote = Emote{
                    .name = EmoteName{u"chatterino:" % tooltip},
                    .images =
                        ImageSet{
                            Image::fromUrl(
                                Url{jsonBadge.value("image1").toString()}, 1.0,
                                baseSize),
                            Image::fromUrl(
                                Url{jsonBadge.value("image2").toString()}, 0.5,
                                baseSize * 2),
                            Image::fromUrl(
                                Url{jsonBadge.value("image3").toString()}, 0.25,
                                baseSize * 4),
                        },
                    .tooltip = Tooltip{tooltip},
                    .homePage = Url{},
                };

                newEmotes.push_back(
                    std::make_shared<const Emote>(std::move(emote)));

                for (const auto &user : jsonBadge.value("users").toArray())
                {
                    newBadgeMap[user.toString()] = index;
                }
                ++index;
            }

            if (newEmotes.empty())
            {
                // an answer without badges: keep what was loaded before
                if (onDone)
                {
                    onDone(false);
                }
                return;
            }

            {
                std::unique_lock lock(this->mutex_);

                // The Kick mapping points into the list of badges. Keep the
                // entries whose badge still exists, at its new place.
                std::unordered_map<uint64_t, int> newKickMapping;
                for (const auto &[kickID, oldIndex] : this->kickMapping)
                {
                    const auto &name = this->emotes[oldIndex]->name;
                    for (int i = 0; i < static_cast<int>(newEmotes.size()); i++)
                    {
                        if (newEmotes[i]->name == name)
                        {
                            newKickMapping.emplace(kickID, i);
                            break;
                        }
                    }
                }

                this->emotes = std::move(newEmotes);
                this->badgeMap = std::move(newBadgeMap);
                this->kickMapping = std::move(newKickMapping);
            }

            WindowManager::notifyBadgesUpdated();
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

}  // namespace chatterino
