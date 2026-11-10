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

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

namespace chatterino {

void shoutoutRaider(const ChannelPtr &channel, const QString &raiderLogin)
{
    if (!getSettings()->autoShoutoutRaids || channel == nullptr ||
        !channel->hasModRights())
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
    if (recent.contains(key) && recent.value(key).secsTo(now) < 10 * 60)
    {
        return;
    }
    recent.insert(key, now);

    // Several mods may have this on in the same chat. Each waits a different
    // moment, and whoever goes first is the only one who sends it.
    const int waitMs = 2000 + int(QRandomGenerator::global()->bounded(3000));
    std::weak_ptr<Channel> weak = channel;
    QTimer::singleShot(waitMs, qApp, [weak, login] {
        auto channel = weak.lock();
        if (channel == nullptr || !channel->hasModRights() ||
            !getSettings()->autoShoutoutRaids)
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
