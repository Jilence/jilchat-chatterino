// SPDX-License-Identifier: MIT

#include "controllers/notifications/DesktopPresenceController.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"

#include <pajlada/settings/setting.hpp>
#include <QDateTime>
#include <QJsonObject>

namespace chatterino {
namespace {

using namespace literals;

constexpr auto AUTH_URL = "https://api.jil.chat/v1/auth/desktop";
constexpr auto HEARTBEAT_URL = "https://api.jil.chat/v1/presence/heartbeat";
constexpr auto PRESENCE_URL = "https://api.jil.chat/v1/presence";
constexpr int HEARTBEAT_INTERVAL_MS = 30'000;
constexpr int HEARTBEAT_TTL_SECONDS = 90;

std::string accountPath(const QString &id, const char *name)
{
    return QString(u"/accounts/uid" % id % u"/" %
                   QString::fromLatin1(name))
        .toStdString();
}

bool enabledFor(const QString &id)
{
    return pajlada::Settings::Setting<bool>::get(
        accountPath(id, "jilchatDesktopPresenceEnabled"));
}

void setAccountValue(const QString &id, const char *name, const QString &value)
{
    pajlada::Settings::Setting<QString>::set(accountPath(id, name), value);
}

}  // namespace

DesktopPresenceController::DesktopPresenceController()
{
}

bool DesktopPresenceController::isEnabled(const QString &twitchID) const
{
    return enabledFor(twitchID);
}

QString DesktopPresenceController::statusText(const QString &twitchID) const
{
    auto it = this->presences_.find(twitchID);
    return it == this->presences_.end() ? QString{} : it->second->status;
}

DesktopPresenceController::Presence &
    DesktopPresenceController::ensurePresence(
        const std::shared_ptr<TwitchAccount> &account)
{
    const auto &id = account->getUserId();
    auto [it, inserted] = this->presences_.try_emplace(id);
    if (inserted)
    {
        it->second = std::make_unique<Presence>();
        auto &presence = *it->second;
        presence.account = account;
        presence.jwt = pajlada::Settings::Setting<QString>::get(
            accountPath(id, "jilchatDesktopPresenceJwt"));
        presence.expiresAt = pajlada::Settings::Setting<QString>::get(
                                 accountPath(
                                     id,
                                     "jilchatDesktopPresenceJwtExpiresAt"))
                                 .toLongLong();
        presence.timer.setInterval(HEARTBEAT_INTERVAL_MS);
        QObject::connect(&presence.timer, &QTimer::timeout, this,
                         [this, id] {
                             auto found = this->presences_.find(id);
                             if (found == this->presences_.end())
                             {
                                 return;
                             }
                             auto account = found->second->account.lock();
                             if (account)
                             {
                                 this->heartbeat(account, *found->second);
                             }
                         });
    }
    else
    {
        it->second->account = account;
    }
    return *it->second;
}

void DesktopPresenceController::start()
{
    for (const auto &account : getApp()->getAccounts()->twitch.accounts)
    {
        if (enabledFor(account->getUserId()))
        {
            this->activate(account);
        }
    }
}

void DesktopPresenceController::setEnabled(
    const std::shared_ptr<TwitchAccount> &account, bool enabled)
{
    if (!account || account->isAnon())
    {
        return;
    }
    pajlada::Settings::Setting<bool>::set(
        accountPath(account->getUserId(),
                    "jilchatDesktopPresenceEnabled"),
        enabled);
    getSettings()->requestSave();
    if (enabled)
    {
        this->activate(account);
    }
    else
    {
        this->remove(account, false);
    }
    this->changed.invoke();
}

void DesktopPresenceController::activate(
    const std::shared_ptr<TwitchAccount> &account)
{
    auto &presence = this->ensurePresence(account);
    presence.stopped = false;
    if (!presence.failedTwitchToken.isEmpty() &&
        presence.failedTwitchToken == account->getOAuthToken())
    {
        return;
    }
    if (!presence.failedTwitchToken.isEmpty())
    {
        presence.failedTwitchToken.clear();
        presence.refreshing = false;
    }
    if (presence.expiresAt > QDateTime::currentSecsSinceEpoch() + 60 &&
        !presence.jwt.isEmpty())
    {
        presence.timer.start();
        this->heartbeat(account, presence);
        return;
    }
    this->authenticate(account, presence);
}

void DesktopPresenceController::accountRemoved(
    const std::shared_ptr<TwitchAccount> &account)
{
    if (!account)
    {
        return;
    }
    const auto id = account->getUserId();
    this->remove(account, true);
    pajlada::Settings::SettingManager::gRemoveSetting(
        QString(u"/accounts/uid" % id).toStdString());
    getSettings()->requestSave();
    this->changed.invoke();
}

void DesktopPresenceController::authenticate(
    const std::shared_ptr<TwitchAccount> &account, Presence &presence)
{
    if (this->shuttingDown_ || presence.authenticating || presence.stopped)
    {
        return;
    }
    presence.authenticating = true;
    const auto id = account->getUserId();
    NetworkRequest(AUTH_URL, NetworkRequestType::Post)
        .json(QJsonObject{{"twitch_token"_L1, account->getOAuthToken()}})
        .hideRequestBody()
        .timeout(10'000)
        .caller(this)
        .onSuccess([this, id](const NetworkResult &result) {
            auto it = this->presences_.find(id);
            if (it == this->presences_.end())
            {
                return;
            }
            auto &p = *it->second;
            auto account = p.account.lock();
            const auto json = result.parseJson();
            p.jwt = json["jwt"_L1].toString();
            const auto expiresIn = json["expires_in"_L1].toInteger();
            if (!account || p.jwt.isEmpty() || expiresIn <= 0 || p.stopped)
            {
                p.failedTwitchToken = account ? account->getOAuthToken() : QString{};
                p.status = "Desktop presence could not be enabled.";
                this->changed.invoke();
                return;
            }
            p.expiresAt = QDateTime::currentSecsSinceEpoch() + expiresIn;
            p.failedTwitchToken.clear();
            p.status.clear();
            setAccountValue(id, "jilchatDesktopPresenceJwt", p.jwt);
            pajlada::Settings::Setting<QString>::set(
                accountPath(id, "jilchatDesktopPresenceJwtExpiresAt"),
                QString::number(p.expiresAt));
            getSettings()->requestSave();
            p.timer.start();
            this->heartbeat(account, p);
            this->changed.invoke();
        })
        .onError([this, id](const NetworkResult &result) {
            auto it = this->presences_.find(id);
            if (it == this->presences_.end())
            {
                return;
            }
            auto &p = *it->second;
            auto account = p.account.lock();
            p.timer.stop();
            if (result.status() == 400 &&
                result.getData().contains(
                    "No JilChat account for this Twitch login"))
            {
                p.status = "Please sign in to the JilChat app once.";
                p.failedTwitchToken =
                    account ? account->getOAuthToken() : QString{};
            }
            else if (result.status() == 403)
            {
                qCCritical(chatterinoApp)
                    << "JilChat desktop authentication was rejected with 403; stopping presence";
                p.status = "Desktop presence is unavailable due to a configuration error.";
                p.failedTwitchToken =
                    account ? account->getOAuthToken() : QString{};
            }
            else
            {
                p.status = "Desktop presence could not be enabled.";
                if (p.refreshing)
                {
                    p.failedTwitchToken =
                        account ? account->getOAuthToken() : QString{};
                }
            }
            this->changed.invoke();
        })
        .finally([this, id] {
            auto it = this->presences_.find(id);
            if (it != this->presences_.end())
            {
                it->second->authenticating = false;
            }
        })
        .execute();
}

void DesktopPresenceController::heartbeat(
    const std::shared_ptr<TwitchAccount> &account, Presence &presence)
{
    if (this->shuttingDown_ || presence.stopped || presence.jwt.isEmpty())
    {
        return;
    }
    const auto id = account->getUserId();
    NetworkRequest(HEARTBEAT_URL, NetworkRequestType::Post)
        .header("Authorization", "Bearer " + presence.jwt)
        .json(QJsonObject{{"ttl_seconds"_L1, HEARTBEAT_TTL_SECONDS},
                          {"client"_L1, "chatterino"},
                          {"version"_L1, Version::instance().version()}})
        .timeout(10'000)
        .caller(this)
        .onError([this, id](const NetworkResult &result) {
            auto it = this->presences_.find(id);
            if (it == this->presences_.end())
            {
                return;
            }
            auto &p = *it->second;
            if (result.status() == 401)
            {
                if (p.refreshing)
                {
                    p.timer.stop();
                    p.stopped = true;
                    if (auto account = p.account.lock())
                    {
                        p.failedTwitchToken = account->getOAuthToken();
                    }
                    return;
                }
                p.refreshing = true;
                p.timer.stop();
                p.jwt.clear();
                p.expiresAt = 0;
                setAccountValue(id, "jilchatDesktopPresenceJwt", {});
                pajlada::Settings::Setting<QString>::set(
                    accountPath(id, "jilchatDesktopPresenceJwtExpiresAt"),
                    {});
                getSettings()->requestSave();
                if (auto account = p.account.lock())
                {
                    this->authenticate(account, p);
                }
            }
            else if (result.status() == 403)
            {
                qCCritical(chatterinoApp)
                    << "JilChat presence JWT was rejected with 403; stopping presence";
                p.timer.stop();
                p.stopped = true;
            }
        })
        .execute();
}

void DesktopPresenceController::sendDelete(const QString &jwt)
{
    if (jwt.isEmpty())
    {
        return;
    }
    NetworkRequest(PRESENCE_URL, NetworkRequestType::Delete)
        .header("Authorization", "Bearer " + jwt)
        .timeout(2'000)
        .caller(this)
        .execute();
}

void DesktopPresenceController::remove(
    const std::shared_ptr<TwitchAccount> &account, bool clearData)
{
    if (!account)
    {
        return;
    }
    const auto id = account->getUserId();
    auto it = this->presences_.find(id);
    if (it != this->presences_.end())
    {
        it->second->timer.stop();
        it->second->stopped = true;
        this->sendDelete(it->second->jwt);
        it->second->jwt.clear();
        it->second->expiresAt = 0;
        if (clearData)
        {
            this->presences_.erase(it);
        }
    }
    setAccountValue(id, "jilchatDesktopPresenceJwt", {});
    pajlada::Settings::Setting<QString>::set(
        accountPath(id, "jilchatDesktopPresenceJwtExpiresAt"), {});
}

void DesktopPresenceController::shutdown()
{
    this->shuttingDown_ = true;
    for (auto &entry : this->presences_)
    {
        auto &presence = entry.second;
        presence->timer.stop();
        presence->stopped = true;
        this->sendDelete(presence->jwt);
    }
}

}  // namespace chatterino
