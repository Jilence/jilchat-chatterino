// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"

#include <IrcMessage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::recentmessages::detail {

std::vector<Communi::IrcMessage *> parseRecentMessages(
    const QJsonObject &jsonRoot);

std::vector<MessagePtr> buildRecentMessages(
    std::vector<Communi::IrcMessage *> &messages, Channel *channel);

/// Builds the messages of a service's answer (its "messages" array) a few at
/// a time, between other things the interface has to do, and hands them to
/// `onBuilt` once all are done. Histories are built one after the other.
/// Must be called from the GUI thread; `onBuilt` is called there too. It
/// isn't called if the channel is gone by then.
void buildRecentMessagesBatched(
    QJsonArray messages, std::weak_ptr<Channel> channel,
    std::function<void(std::vector<MessagePtr>)> onBuilt);

/// Puts what several services know of a channel's history together: the
/// messages of all `batches` in the order they were sent, each only once,
/// and at most the `limit` newest ones. Taken from Moltorino.
QJsonArray mergeRecentMessageBatches(const std::vector<QJsonArray> &batches,
                                     int limit);

QString recentMessagesApiUrlTemplate();

/// The URL templates of the services to ask, in the order to ask them: the
/// preferred one first. Only one if CHATTERINO2_RECENT_MESSAGES_URL is set.
std::vector<QString> recentMessagesApiUrlTemplates();

QUrl constructRecentMessagesUrl(
    const QString &name, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before);

/// The same for the service with `urlTemplate`
QUrl constructRecentMessagesUrl(
    const QString &urlTemplate, const QString &name, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before);

}  // namespace chatterino::recentmessages::detail
