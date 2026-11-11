// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/RaidShoutout.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace chatterino {

namespace {

/// True when the chat is in the selected tab of a window, not just open in
/// some other tab
bool isChatOnScreen(const Channel &channel)
{
    const auto visible = getApp()->getWindows()->getVisibleChannelNames();
    return std::ranges::any_of(visible, [&channel](const QString &name) {
        return name.compare(channel.getName(), Qt::CaseInsensitive) == 0;
    });
}

}  // namespace

void shoutoutRaider(const ChannelPtr &channel, const QString &raiderLogin)
{
    // Only for a chat you're looking at, so it never speaks in a chat you
    // just have open in the background
    if (!getSettings()->autoShoutoutRaids || channel == nullptr ||
        !channel->hasModRights() || !isChatOnScreen(*channel))
    {
        return;
    }
    const auto login = raiderLogin.trimmed().toLower();
    static const QRegularExpression validLogin(
        QStringLiteral("^[a-z0-9_]{1,25}$"));
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!validLogin.match(login).hasMatch() || account->isAnon() ||
        account->getUserName().compare(login, Qt::CaseInsensitive) == 0)
    {
        return;
    }

    // Twitch can repeat a raid notice; one shoutout per raider is enough
    static QHash<QString, QDateTime> recent;
    const auto key = channel->getName().toLower() + u'/' + login;
    const auto now = QDateTime::currentDateTimeUtc();
    if (recent.contains(key) && recent.value(key).secsTo(now) < qint64{10 * 60})
    {
        return;
    }
    recent.insert(key, now);

    // Several mods may have this on in the same chat. Each waits a different
    // moment, and whoever goes first is the only one who sends it.
    const int waitMs = 2000 + QRandomGenerator::global()->bounded(3000);
    std::weak_ptr<Channel> weak = channel;
    QTimer::singleShot(waitMs, qApp, [weak, login] {
        auto channel = weak.lock();
        if (channel == nullptr || !channel->hasModRights() ||
            !getSettings()->autoShoutoutRaids || !isChatOnScreen(*channel))
        {
            return;
        }
        for (const auto &message : channel->getMessageSnapshot(80))
        {
            const auto &text = message->messageText;
            const bool shoutout =
                text.startsWith(QStringLiteral("!so"), Qt::CaseInsensitive) ||
                text.startsWith(QStringLiteral("!shoutout"),
                                Qt::CaseInsensitive) ||
                text.startsWith(QStringLiteral("/shoutout"),
                                Qt::CaseInsensitive);
            if (shoutout && text.contains(login, Qt::CaseInsensitive))
            {
                return;
            }
        }

        auto command = getSettings()->raidShoutoutCommand.getValue().trimmed();
        if (command.isEmpty())
        {
            command = QStringLiteral("!so @{user}");
        }
        command.replace(QStringLiteral("{user}"), login, Qt::CaseInsensitive);
        // Through the same path as typing it, so /shoutout works too
        const auto text =
            getApp()->getCommands()->execCommand(command, channel, false);
        if (!text.isEmpty())
        {
            channel->sendMessage(text);
        }
    });
}

}  // namespace chatterino
