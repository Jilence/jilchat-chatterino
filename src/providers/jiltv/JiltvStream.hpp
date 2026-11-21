// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace chatterino {

/// One JilTV event stream (server-sent events), e.g.
/// /v1/events/channels/twitch/{id} or /v1/events/cosmetics. Reconnects on
/// its own with backoff and resumes after the last event it saw
/// (Last-Event-ID), so nothing is missed across short drops. GUI thread.
class JiltvStream
{
public:
    using Handler =
        std::function<void(const QString &type, const QJsonObject &data)>;

    JiltvStream(QNetworkAccessManager *network, QUrl url, Handler handler);
    ~JiltvStream();

    JiltvStream(const JiltvStream &) = delete;
    JiltvStream &operator=(const JiltvStream &) = delete;

    const QUrl &url() const;

private:
    void connect();
    void onData();
    void onFinished();
    void dispatch();

    QNetworkAccessManager *network_;
    QUrl url_;
    Handler handler_;
    /// Context for the reply's and timers' connections.
    QObject context_;
    QPointer<QNetworkReply> reply_;
    QTimer retry_;
    /// No byte for this long (the server sends a heartbeat every 25 s) means
    /// the connection is dead even if the socket doesn't say so.
    QTimer watchdog_;
    int backoffMs_ = 1000;
    bool stopped_ = false;

    QByteArray buffer_;
    QString lastId_;
    QString event_;
    QByteArray data_;
};

}  // namespace chatterino
