// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvPersonalEmotes.hpp"

#include "messages/Emote.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

namespace chatterino {

BttvPersonalEmotes &BttvPersonalEmotes::instance()
{
    static BttvPersonalEmotes instance;
    return instance;
}

void BttvPersonalEmotes::setUserEmotes(const QString &userId,
                                       const QJsonArray &emotes)
{
    if (userId.isEmpty())
    {
        return;
    }

    {
        const std::scoped_lock lock(this->mutex_);
        const auto it = this->listed_.find(userId);
        if (emotes.isEmpty())
        {
            if (it != this->listed_.end())
            {
                this->listed_.erase(it);
                this->emotes_.erase(userId);
            }
            return;
        }
        if (it != this->listed_.end() && it->second == emotes)
        {
            return;
        }
        this->listed_[userId] = emotes;
    }

    // The images of the emotes belong to the GUI thread.
    runInGuiThread([this, userId, emotes] {
        auto map = std::make_shared<const EmoteMap>(
            bttv::detail::parsePersonalEmotes(emotes));

        const std::scoped_lock lock(this->mutex_);
        // Unless BetterTTV has listed something else by now.
        const auto it = this->listed_.find(userId);
        if (it != this->listed_.end() && it->second == emotes)
        {
            this->emotes_[userId] = std::move(map);
        }
    });
}

std::optional<EmotePtr> BttvPersonalEmotes::getEmote(const QString &userId,
                                                     EmoteNameView name) const
{
    const auto emotes = this->getEmotes(userId);
    if (emotes == nullptr)
    {
        return std::nullopt;
    }
    const auto it = emotes->find(name);
    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::shared_ptr<const EmoteMap> BttvPersonalEmotes::getEmotes(
    const QString &userId) const
{
    if (!getSettings()->enableBTTVPersonalEmotes)
    {
        return nullptr;
    }

    const std::scoped_lock lock(this->mutex_);
    const auto it = this->emotes_.find(userId);
    if (it == this->emotes_.end())
    {
        return nullptr;
    }
    return it->second;
}

}  // namespace chatterino
