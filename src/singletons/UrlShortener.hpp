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
    void shorten(const QString &url, const ChannelPtr &channel,
                 const QPointer<ResizingTextEdit> &textEdit,
                 const QTextCursor &cursor);

private:
    static void sendShortenRequest(const QString &url,
                                   const ChannelPtr &channel,
                                   const QPointer<ResizingTextEdit> &textEdit,
                                   const QTextCursor &cursor);

    /// `cursor` selects the pasted link, which is replaced by the short one
    static void handleSuccessfulShorten(
        const NetworkResult &result, const ChannelPtr &channel,
        const QPointer<ResizingTextEdit> &textEdit, QTextCursor cursor,
        const QString &url);

    static void handleFailedShorten(const NetworkResult &result,
                                    const ChannelPtr &channel);
};
}  // namespace chatterino
