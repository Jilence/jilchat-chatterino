// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/seventv/Presence.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QStringBuilder>

#include <chrono>

using namespace Qt::Literals::StringLiterals;

namespace chatterino::commands {

QString seventvPresence(const CommandContext &ctx)
{
    if (!ctx.channel)
    {
        return {};
    }
    if (!ctx.kickChannel && !ctx.twitchChannel)
    {
        ctx.channel->addSystemMessage(
            "/seventv-presence must be used in a Twitch or Kick channel.");
        return {};
    }

    constinit static auto lastUse =
        std::chrono::system_clock::time_point::min();
    constexpr std::chrono::seconds cooldown(10);

    auto now = std::chrono::system_clock::now();
    if (lastUse > now - cooldown)
    {
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(
            cooldown - (now - lastUse));
        ctx.channel->addSystemMessage("Command is on cooldown, try in " %
                                      QString::number(diff.count()) %
                                      "s again.");
        return {};
    }
    lastUse = now;

    QString userArg;
    if (ctx.words.size() >= 2)
    {
        userArg = ctx.words.at(1);
    }

    auto reply = [weak = ctx.channel->weak_from_this()](const QString &msg) {
        if (auto chan = weak.lock())
        {
            chan->addSystemMessage(msg);
        }
    };
    QString platformID;
    if (ctx.twitchChannel)
    {
        platformID = ctx.twitchChannel->roomId();
    }
    else if (ctx.kickChannel)
    {
        platformID = QString::number(ctx.kickChannel->userID());
    }

    auto run = [reply, platformID](const QString &platform,
                                   const QString &stvID) {
        getApp()->getSeventvAPI()->updatePresence(
            platform, platformID, stvID,
            [reply] {
                reply(u"Updated presence."_s);
            },
            [reply](const NetworkResult &res) {
                reply(u"Failed to update presence: " % res.formatError());
            });
    };

    if (!userArg.isEmpty())
    {
        if (ctx.twitchChannel)
        {
            getHelix()->getUserByName(
                userArg,
                [run, reply](const auto &user) {
                    getApp()->getSeventvAPI()->getUserByTwitchID(
                        user.id,
                        [run](const auto &obj, const auto & /*raw*/) {
                            run(u"TWITCH"_s,
                                obj["user"_L1]["id"_L1].toString());
                        },
                        [reply](const NetworkResult &err) {
                            reply(u"Failed to find 7TV user: " %
                                  err.formatError());
                        });
                },
                [reply] {
                    reply(u"Failed to find user."_s);
                });
        }
        else if (ctx.kickChannel)
        {
            KickApi::privateChannelInfo(userArg, [run, reply](const auto &res) {
                if (!res)
                {
                    reply(u"Failed to find user: " % res.error());
                    return;
                }
                getApp()->getSeventvAPI()->getUserByKickID(
                    res->user.userID,
                    [run](const auto &obj, const auto & /*raw*/) {
                        run(u"KICK"_s, obj["user"_L1]["id"_L1].toString());
                    },
                    [reply](const NetworkResult &err) {
                        reply(u"Failed to find 7TV user: " % err.formatError());
                    });
            });
        }

        return {};
    }

    if (ctx.twitchChannel)
    {
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        if (user->isAnon())
        {
            ctx.channel->addSystemMessage(
                u"You must be logged in to update your presence."_s);
            return {};
        }
        if (user->getSeventvUserID().isEmpty())
        {
            ctx.channel->addSystemMessage(
                u"Your Twitch account is not connected with 7TV"_s);
            return {};
        }
        run(u"TWITCH"_s, user->getSeventvUserID());
    }
    else if (ctx.kickChannel)
    {
        auto user = getApp()->getAccounts()->kick.current();
        if (user->isAnonymous())
        {
            ctx.channel->addSystemMessage(
                u"You must be logged in to update your presence."_s);
            return {};
        }
        if (user->seventvUserID().isEmpty())
        {
            ctx.channel->addSystemMessage(
                u"Your Kick account is not connected with 7TV"_s);
            return {};
        }
        run(u"KICK"_s, user->seventvUserID());
    }

    return {};
}

}  // namespace chatterino::commands
