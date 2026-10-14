// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ReloadBadges.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/chatsen/ChatsenBadges.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/dankchat/DankChatBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffzap/FfzApBadges.hpp"
#include "providers/folhinha/FolhinhaBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/jilchat/JilChatBadges.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QString>
#include <QStringBuilder>

#include <functional>
#include <utility>

namespace {

using namespace chatterino;

/// A callback that tells `channel` how reloading `what` went. It may be
/// called from any thread.
std::function<void(bool)> reloadMessage(const ChannelPtr &channel, QString what)
{
    return [weak = std::weak_ptr<Channel>(channel),
            what = std::move(what)](bool success) {
        runInGuiThread([weak, what, success] {
            auto channel = weak.lock();
            if (!channel)
            {
                return;
            }

            if (success)
            {
                channel->addSystemMessage(what % u" reloaded.");
            }
            else
            {
                channel->addSystemMessage(u"Failed to reload " % what % u'.');
            }
        });
    };
}

}  // namespace

namespace chatterino {

void reloadBadgeProviders(const ChannelPtr &channel)
{
    auto *app = getApp();
    auto *settings = getSettings();

    if (settings->showBadgesHomiesSupporter || settings->showBadgesHomiesCustom)
    {
        app->getHomiesBadges()->loadHomiesBadges(
            reloadMessage(channel, "Homies badges"));
    }
    if (settings->showBadgesFolhinha)
    {
        app->getFolhinhaBadges()->loadFolhinhaBadges(
            reloadMessage(channel, "FolhinhaBot badges"));
    }
    if (settings->showBadgesMoltorino)
    {
        if (auto *moltorino = app->getMoltorinoSupporterBadges())
        {
            moltorino->refreshNow(reloadMessage(channel, "Moltorino badges"));
        }
    }
    if (settings->showBadgesJilChat)
    {
        app->getJilChatBadges()->loadJilChatBadges(
            reloadMessage(channel, "JilChat badges"));
    }
    if (settings->showBadgesBluzyrino)
    {
        app->getBluzyrinoBadges()->loadBluzyrinoBadges(
            reloadMessage(channel, "Bluzyrino badges"));
    }
    if (settings->showBadgesDankChat)
    {
        app->getDankChatBadges()->load(
            reloadMessage(channel, "DankChat badges"));
    }
    if (settings->showBadgesChatterino)
    {
        app->getChatterinoBadges()->reload(
            reloadMessage(channel, "Chatterino badges"));
    }
    if (settings->showBadgesFfz)
    {
        app->getFfzBadges()->load(
            reloadMessage(channel, "FrankerFaceZ badges"));
    }
    if (settings->showBadgesFfzAp)
    {
        app->getFfzApBadges()->load(reloadMessage(channel, "FFZ:AP badges"));
    }
    if (settings->showBadgesChatsen)
    {
        app->getChatsenBadges()->load(reloadMessage(channel, "Chatsen badges"));
    }
}

}  // namespace chatterino
