// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QPointer>
#include <QString>
#include <QTextCursor>

#include <memory>

namespace chatterino {

class ResizingTextEdit;
class Channel;
class NetworkResult;

using ChannelPtr = std::shared_ptr<Channel>;

class UrlShortener final
{
public:
    void shorten(const QString &url, ChannelPtr channel,
                 QPointer<ResizingTextEdit> textEdit, QTextCursor cursor);

private:
    void sendShortenRequest(const QString &url, ChannelPtr channel,
                            QPointer<ResizingTextEdit> textEdit,
                            QTextCursor cursor);

    void handleSuccessfulShorten(const NetworkResult &result,
                                 ChannelPtr channel,
                                 QPointer<ResizingTextEdit> textEdit,
                                 QTextCursor cursor, const QString &url);

    void handleFailedShorten(const NetworkResult &result, ChannelPtr channel);
};
}  // namespace chatterino
