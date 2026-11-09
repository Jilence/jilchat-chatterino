// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ShareWatchStreak.hpp"

#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QInputDialog>
#include <QLineEdit>

namespace chatterino {

void shareWatchStreakIn(const ChannelPtr &channel, const QString &message)
{
    auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
    if (twitch == nullptr)
    {
        if (channel != nullptr)
        {
            channel->addSystemMessage(
                "Watch streaks can only be shared in a Twitch channel.");
        }
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (auth.token.isEmpty())
    {
        twitch->addSystemMessage(
            MoltorinoAuth::authRequiredMessage("share your watch streak"));
        return;
    }

    std::weak_ptr<Channel> weak = twitch;
    TwitchGql::shareWatchStreak(
        twitch->roomId(), message, auth.token,
        [weak](TwitchGql::WatchStreakShare result) {
            auto channel = weak.lock();
            if (channel == nullptr)
            {
                return;
            }
            if (result == TwitchGql::WatchStreakShare::Shared)
            {
                // Twitch posts the streak in chat itself, so it shows up there
                channel->addSystemMessage("Shared your watch streak!");
            }
            else if (result == TwitchGql::WatchStreakShare::AlreadyShared)
            {
                channel->addSystemMessage(
                    "You've already shared your current watch streak here.");
            }
            else
            {
                channel->addSystemMessage(
                    "There's no watch streak to share right now. Twitch "
                    "gives you one after watching several streams in a row, "
                    "and it can be shared once per stream.");
            }
        },
        [weak](const QString &error) {
            if (auto channel = weak.lock())
            {
                channel->addSystemMessage(MoltorinoAuth::normalizeAuthError(
                    "share your watch streak", error));
            }
        });
}

void askAndShareWatchStreak(QWidget *parent, const ChannelPtr &channel)
{
    bool ok = false;
    const auto text = QInputDialog::getText(
        parent, "Share your watch streak",
        "Add a message to go with it (optional):", QLineEdit::Normal, {}, &ok);
    if (!ok)
    {
        return;
    }
    shareWatchStreakIn(channel, text);
}

}  // namespace chatterino
