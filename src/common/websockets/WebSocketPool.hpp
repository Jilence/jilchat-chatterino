// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <memory>
#include <vector>

namespace chatterino::ws::detail {
class WebSocketPoolImpl;
class WebSocketConnection;
}  // namespace chatterino::ws::detail

namespace chatterino {

class WebSocketHandle
{
public:
    WebSocketHandle() = default;
    WebSocketHandle(std::weak_ptr<ws::detail::WebSocketConnection> conn);
    ~WebSocketHandle();

    WebSocketHandle(const WebSocketHandle &) = delete;
    WebSocketHandle(WebSocketHandle &&) = default;
    WebSocketHandle &operator=(const WebSocketHandle &) = delete;
    WebSocketHandle &operator=(WebSocketHandle &&) = default;

    void sendText(const QByteArray &data);
    void sendBinary(const QByteArray &data);
    void close();
    /// Whether this still belongs to a connection
    [[nodiscard]] bool isValid() const;

private:
    std::weak_ptr<ws::detail::WebSocketConnection> conn;
};

struct WebSocketListener {
    virtual ~WebSocketListener() = default;

    virtual void onOpen() = 0;

    virtual void onTextMessage(QByteArray data) = 0;

    virtual void onBinaryMessage(QByteArray data) = 0;

    virtual void onClose(std::unique_ptr<WebSocketListener> self) = 0;
};

struct WebSocketOptions {
    QUrl url;
    std::vector<std::pair<std::string, std::string>> headers;
    /// The largest message to accept, 0 for Beast's default
    std::size_t maxMessageBytes = 0;
};

class WebSocketPool
{
public:
    WebSocketPool(QString shortName = {});
    ~WebSocketPool();

    [[nodiscard]] WebSocketHandle createSocket(
        WebSocketOptions options, std::unique_ptr<WebSocketListener> listener);

private:
    std::unique_ptr<ws::detail::WebSocketPoolImpl> impl;
    QString shortName;
};

}  // namespace chatterino
