// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <compare>
#include <functional>
#include <vector>

namespace chatterino {

class NetworkResult;
class TwitchChannel;

}  // namespace chatterino

/// logs.zonian.dev: the public chat logs behind tv.supa.sh. Used to load older
/// messages on usercards, at the top of the chat and in the search.
namespace chatterino::publiclogs {

/// A month (day == 0) or a day that has logs.
struct LogDate {
    int year = 0;
    int month = 0;
    int day = 0;

    auto operator<=>(const LogDate &) const = default;

    QDate firstDay() const;
    QDate lastDay() const;
};

/// Which months (with a user) or days (without) have logs in the channel.
QUrl listUrl(const QString &channel, const QString &user = {});
/// The messages of a user in a channel in one month. With a `limit`, one page
/// of them, newest first, starting `offset` messages from the newest.
QUrl userMonthUrl(const QString &channel, const QString &user, LogDate month,
                  int limit = 0, int offset = 0);
/// All messages in a channel on one day.
QUrl channelDayUrl(const QString &channel, LogDate day);
/// Up to `limit` messages in a channel between `from` and `to`, newest first.
QUrl channelRangeUrl(const QString &channel, const QDateTime &from,
                     const QDateTime &to, int limit);

/// The dates of a listUrl response, newest first.
std::vector<LogDate> parseLogDates(const NetworkResult &result);
/// The log lines of a message response.
QJsonArray parseLogLines(const NetworkResult &result);

/// Builds chat messages from log lines like the recent messages: faded, and
/// marked historical so they don't ping. Returns them oldest first. Lines
/// `accept` rejects are skipped before building, which is the expensive part.
std::vector<MessagePtr> buildMessages(
    const QJsonArray &lines, TwitchChannel *channel, bool newestFirst,
    const std::function<bool(const QJsonObject &)> &accept = {});

/// A day separator like the ones between the recent messages.
MessagePtr makeDaySeparator(QDate day);

/// Sends a request to logs.zonian.dev. The requests of the whole app share a
/// client-side rate limit matching the service (which answers HTTP 429 when
/// flooded), and are retried if they were rate limited anyway. `stillWanted`
/// is checked before sending, so requests nobody needs anymore are dropped
/// without using up the limit.
void get(const QUrl &url, int timeoutMs,
         std::function<void(const NetworkResult &)> onSuccess,
         std::function<void(const NetworkResult &)> onError,
         std::function<bool()> stillWanted = {});

}  // namespace chatterino::publiclogs
