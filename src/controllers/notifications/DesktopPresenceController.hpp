// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <pajlada/signals.hpp>
#include <QObject>
#include <QTimer>

#include <memory>
#include <unordered_map>

namespace chatterino {

class TwitchAccount;

class DesktopPresenceController final : public QObject
{
public:
    DesktopPresenceController();

    bool isEnabled(const QString &twitchID) const;
    QString statusText(const QString &twitchID) const;
    void setEnabled(const std::shared_ptr<TwitchAccount> &account, bool enabled);
    void accountRemoved(const std::shared_ptr<TwitchAccount> &account);
    void start();
    void shutdown();

    pajlada::Signals::NoArgSignal changed;

private:
    struct Presence {
        std::weak_ptr<TwitchAccount> account;
        QString jwt;
        qint64 expiresAt{};
        QString failedTwitchToken;
        QString status;
        QTimer timer;
        bool authenticating{};
        bool refreshing{};
        bool stopped{};
    };

    std::unordered_map<QString, std::unique_ptr<Presence>> presences_;
    bool shuttingDown_{};

    Presence &ensurePresence(const std::shared_ptr<TwitchAccount> &account);
    void activate(const std::shared_ptr<TwitchAccount> &account);
    void authenticate(const std::shared_ptr<TwitchAccount> &account,
                      Presence &presence);
    void heartbeat(const std::shared_ptr<TwitchAccount> &account,
                   Presence &presence);
    void remove(const std::shared_ptr<TwitchAccount> &account, bool clearData);
    void sendDelete(const QString &jwt);
};

}  // namespace chatterino
