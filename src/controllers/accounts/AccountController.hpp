// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVector.hpp"
#include "providers/kick/KickAccountManager.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"

#include <QObject>
#include <memory>

namespace chatterino {

class Account;
class Settings;
class Paths;

class AccountModel;
class DesktopPresenceController;

class AccountController final
{
public:
    AccountController();
    ~AccountController();

    AccountModel *createModel(QObject *parent);

    void load();
    DesktopPresenceController &desktopPresence();

    TwitchAccountManager twitch;
    KickAccountManager kick;

private:
    SignalVector<std::shared_ptr<Account>> accounts_;
    std::unique_ptr<DesktopPresenceController> desktopPresence_;
};

}  // namespace chatterino
