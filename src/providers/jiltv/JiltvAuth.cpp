// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jiltv/JiltvAuth.hpp"

#include "Application.hpp"
#include "common/Credentials.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/jiltv/Jiltv.hpp"
#include "providers/twitch/TwitchAccount.hpp"

#include <QJsonDocument>

namespace {

using namespace chatterino;

/// The JilChat Chatterino app on jiltv.app. Public, there is no secret.
constexpr auto CLIENT_ID = "01M64W4045EXJBPZSSEBWHPZ39";
constexpr auto SCOPE = "COSMETICS_WRITE";
constexpr auto CREDENTIAL_PROVIDER = "jiltv";
/// Renew this long before the access token expires
constexpr qint64 REFRESH_EARLY_SECS = 60;

QString currentAccount()
{
    return getApp()->getAccounts()->twitch.getCurrent()->getUserId();
}

QString endpoint(const QString &path)
{
    return Jiltv::instance().apiUrl() + path;
}

/// The `error` of an OAuth error answer, empty when there is none.
QString oauthError(const NetworkResult &result)
{
    return result.parseJson().value("error").toString();
}

}  // namespace

namespace chatterino {

JiltvAuth &JiltvAuth::instance()
{
    static JiltvAuth auth;
    return auth;
}

void JiltvAuth::load(QObject *receiver, std::function<void()> done)
{
    const auto account = currentAccount();
    if (this->loaded_ && this->account_ == account)
    {
        done();
        return;
    }

    Credentials::instance().get(
        QString::fromLatin1(CREDENTIAL_PROVIDER), account, receiver,
        [this, account, done = std::move(done)](const QString &stored) {
            this->account_ = account;
            this->loaded_ = true;
            const auto json = QJsonDocument::fromJson(stored.toUtf8()).object();
            this->accessToken_ = json.value("access_token").toString();
            this->refreshToken_ = json.value("refresh_token").toString();
            this->expiresAt_ = QDateTime::fromSecsSinceEpoch(
                                   json.value("expires_at").toInteger())
                                   .toUTC();
            done();
        });
}

bool JiltvAuth::signedIn() const
{
    return this->loaded_ && this->account_ == currentAccount() &&
           !this->refreshToken_.isEmpty();
}

void JiltvAuth::requestDeviceCode(
    const std::function<void(const DeviceCode &)> &onCode,
    const std::function<void(const QString &)> &onError)
{
    NetworkRequest(endpoint(QStringLiteral("/v1/oauth/device/code")),
                   NetworkRequestType::Post)
        .json(QJsonObject{
            {"client_id", QString::fromLatin1(CLIENT_ID)},
            {"scope", QString::fromLatin1(SCOPE)},
        })
        .timeout(15000)
        .onSuccess([onCode](const NetworkResult &result) {
            const auto json = result.parseJson();
            onCode({
                .deviceCode = json.value("device_code").toString(),
                .userCode = json.value("user_code").toString(),
                .verificationUri = json.value("verification_uri").toString(),
                .verificationUriComplete =
                    json.value("verification_uri_complete").toString(),
                .expiresIn = json.value("expires_in").toInt(600),
                .interval = json.value("interval").toInt(5),
            });
        })
        .onError([onError](const NetworkResult &result) {
            if (result.status() == 429)
            {
                onError(QStringLiteral(
                    "Too many sign-in attempts. Try again in a minute."));
                return;
            }
            const auto error = oauthError(result);
            onError(error.isEmpty() ? result.formatError() : error);
        })
        .execute();
}

void JiltvAuth::poll(
    const QString &deviceCode,
    const std::function<void(PollResult, const QString &)> &done)
{
    NetworkRequest(endpoint(QStringLiteral("/v1/oauth/token")),
                   NetworkRequestType::Post)
        .json(QJsonObject{
            {"grant_type",
             QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")},
            {"client_id", QString::fromLatin1(CLIENT_ID)},
            {"device_code", deviceCode},
        })
        .timeout(15000)
        .onSuccess([this, done](const NetworkResult &result) {
            this->account_ = currentAccount();
            this->loaded_ = true;
            this->store(result.parseJson());
            done(PollResult::Done, {});
        })
        .onError([done](const NetworkResult &result) {
            const auto error = oauthError(result);
            // No answer at all (network) counts as not answered yet: try
            // again with the next poll
            if (error == u"authorization_pending" ||
                (error.isEmpty() && !result.status()))
            {
                done(PollResult::Pending, {});
            }
            else if (error == u"slow_down")
            {
                done(PollResult::SlowDown, {});
            }
            else if (error == u"access_denied")
            {
                done(PollResult::Denied, {});
            }
            else if (error == u"expired_token")
            {
                done(PollResult::Expired, {});
            }
            else
            {
                done(PollResult::Failed,
                     error.isEmpty() ? result.formatError() : error);
            }
        })
        .execute();
}

void JiltvAuth::store(const QJsonObject &tokenResponse)
{
    this->accessToken_ = tokenResponse.value("access_token").toString();
    this->refreshToken_ = tokenResponse.value("refresh_token").toString();
    this->expiresAt_ = QDateTime::currentDateTimeUtc().addSecs(
        tokenResponse.value("expires_in").toInt(3600));

    const QJsonObject stored{
        {"access_token", this->accessToken_},
        {"refresh_token", this->refreshToken_},
        {"expires_at", this->expiresAt_.toSecsSinceEpoch()},
    };
    Credentials::instance().set(
        QString::fromLatin1(CREDENTIAL_PROVIDER), this->account_,
        QString::fromUtf8(
            QJsonDocument(stored).toJson(QJsonDocument::Compact)));
}

void JiltvAuth::clear()
{
    this->accessToken_.clear();
    this->refreshToken_.clear();
    this->expiresAt_ = {};
    Credentials::instance().erase(QString::fromLatin1(CREDENTIAL_PROVIDER),
                                  this->account_);
}

void JiltvAuth::refresh(std::function<void(bool, const QString &)> done)
{
    this->refreshWaiters_.push_back(std::move(done));
    if (this->refreshing_)
    {
        return;
    }
    this->refreshing_ = true;

    const auto finish = [this](bool ok, const QString &error) {
        this->refreshing_ = false;
        auto waiters = std::move(this->refreshWaiters_);
        this->refreshWaiters_.clear();
        for (const auto &waiter : waiters)
        {
            waiter(ok, error);
        }
    };

    NetworkRequest(endpoint(QStringLiteral("/v1/oauth/token")),
                   NetworkRequestType::Post)
        .json(QJsonObject{
            {"grant_type", QStringLiteral("refresh_token")},
            {"client_id", QString::fromLatin1(CLIENT_ID)},
            {"refresh_token", this->refreshToken_},
        })
        .timeout(15000)
        .onSuccess([this, finish](const NetworkResult &result) {
            this->store(result.parseJson());
            finish(true, {});
        })
        .onError([this, finish](const NetworkResult &result) {
            if (oauthError(result) == u"invalid_grant")
            {
                // Revoked or used up: signed out
                this->clear();
                finish(false, QStringLiteral("Your JilTV sign-in ended. Sign "
                                             "in again."));
                return;
            }
            // Temporary (429, 5xx, network): keep the tokens
            finish(false, result.formatError());
        })
        .execute();
}

void JiltvAuth::graphql(const QString &query, const QJsonObject &variables,
                        const std::function<void(const QJsonObject &)> &onData,
                        const std::function<void(const QString &)> &onError)
{
    if (!this->signedIn())
    {
        onError(QStringLiteral("Sign in to JilTV first."));
        return;
    }
    if (this->accessToken_.isEmpty() ||
        QDateTime::currentDateTimeUtc().secsTo(this->expiresAt_) <
            REFRESH_EARLY_SECS)
    {
        this->refresh([this, query, variables, onData, onError](
                          bool ok, const QString &error) {
            if (!ok)
            {
                onError(error);
                return;
            }
            this->sendGraphql(query, variables, true, onData, onError);
        });
        return;
    }
    this->sendGraphql(query, variables, false, onData, onError);
}

void JiltvAuth::sendGraphql(
    const QString &query, const QJsonObject &variables, bool retried,
    const std::function<void(const QJsonObject &)> &onData,
    const std::function<void(const QString &)> &onError)
{
    NetworkRequest(endpoint(QStringLiteral("/v1/graphql")),
                   NetworkRequestType::Post)
        .header("Authorization", "Bearer " + this->accessToken_.toUtf8())
        .json(QJsonObject{{"query", query}, {"variables", variables}})
        .timeout(15000)
        .onSuccess([onData, onError](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto errors = root.value("errors").toArray();
            if (!errors.isEmpty())
            {
                const auto error = errors.first().toObject();
                const auto code = error.value("extensions")
                                      .toObject()
                                      .value("code")
                                      .toString();
                onError(code == u"missing_scope"
                            ? QStringLiteral("JilTV didn't allow this. Sign "
                                             "in again.")
                            : error.value("message").toString(
                                  QStringLiteral("JilTV rejected the "
                                                 "request")));
                return;
            }
            onData(root.value("data").toObject());
        })
        .onError([this, query, variables, retried, onData,
                  onError](const NetworkResult &result) {
            if (result.status() == 401 && !retried)
            {
                // Expired early or revoked: renew once and try again
                this->refresh([this, query, variables, onData, onError](
                                  bool ok, const QString &error) {
                    if (!ok)
                    {
                        onError(error);
                        return;
                    }
                    this->sendGraphql(query, variables, true, onData, onError);
                });
                return;
            }
            onError(result.formatError());
        })
        .execute();
}

void JiltvAuth::signOut()
{
    if (!this->refreshToken_.isEmpty())
    {
        NetworkRequest(endpoint(QStringLiteral("/v1/oauth/revoke")),
                       NetworkRequestType::Post)
            .json(QJsonObject{
                {"client_id", QString::fromLatin1(CLIENT_ID)},
                {"token", this->refreshToken_},
            })
            .timeout(15000)
            .execute();
    }
    this->clear();
}

}  // namespace chatterino
