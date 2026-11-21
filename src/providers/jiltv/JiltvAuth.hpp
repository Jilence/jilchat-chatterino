// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace chatterino {

/// Signing in to JilTV with the OAuth device flow
/// (https://jiltv.app/docs/device-flow), so the app can change the cosmetics
/// of whoever uses it.
///
/// The tokens are kept in the system credential store, per Twitch account.
/// Access tokens last an hour and are renewed with the refresh token, which
/// works once; only one renewal runs at a time.
class JiltvAuth
{
public:
    static JiltvAuth &instance();

    struct DeviceCode {
        QString deviceCode;
        QString userCode;
        QString verificationUri;
        QString verificationUriComplete;
        int expiresIn = 600;
        int interval = 5;
    };

    enum class PollResult : std::uint8_t {
        Pending,
        SlowDown,
        /// Signed in; the tokens are stored
        Done,
        Denied,
        Expired,
        Failed,
    };

    /// Reads the tokens of the current Twitch account from the credential
    /// store, once per account. `done` runs in the GUI thread.
    void load(QObject *receiver, std::function<void()> done);
    bool signedIn() const;

    /// Starts a sign-in. Call it when the user asks to sign in, not in the
    /// background (the API limits it to 20 per minute).
    static void requestDeviceCode(
        const std::function<void(const DeviceCode &)> &onCode,
        const std::function<void(const QString &)> &onError);
    /// Asks once whether the user answered. `error` explains Failed.
    void poll(
        const QString &deviceCode,
        const std::function<void(PollResult, const QString &error)> &done);

    /// A GraphQL request with the access token, renewed when it is about to
    /// expire or the API refuses it.
    void graphql(const QString &query, const QJsonObject &variables,
                 const std::function<void(const QJsonObject &data)> &onData,
                 const std::function<void(const QString &)> &onError);

    /// Revokes the sign-in and forgets the tokens.
    void signOut();

private:
    JiltvAuth() = default;

    void store(const QJsonObject &tokenResponse);
    void clear();
    /// Renews the access token; calls queued while it runs wait for it.
    /// `done` gets false when it failed.
    void refresh(std::function<void(bool ok, const QString &error)> done);
    void sendGraphql(const QString &query, const QJsonObject &variables,
                     bool retried,
                     const std::function<void(const QJsonObject &)> &onData,
                     const std::function<void(const QString &)> &onError);

    /// The Twitch account the tokens belong to
    QString account_;
    bool loaded_ = false;
    QString accessToken_;
    QString refreshToken_;
    QDateTime expiresAt_;

    bool refreshing_ = false;
    std::vector<std::function<void(bool, const QString &)>> refreshWaiters_;
};

}  // namespace chatterino
