// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDate>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <array>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;

namespace wrapped {

/// Counts what you do in chat, day by day, for JilChat Wrapped. Everything
/// stays on this computer, in jilchat-wrapped.json in the settings folder.
class WrappedStats : public QObject
{
public:
    static WrappedStats &instance();

    /// Starts counting time with chats open. Call once the windows exist.
    void start();
    /// Writes anything not saved yet. Call as the app quits.
    void save();

    /// Every message added to a Twitch channel passes through here
    void recordMessage(const QString &channelName, const MessagePtr &message);

    using Ranking = std::vector<std::pair<QString, int>>;

    struct Summary {
        QDate from;
        QDate to;
        /// The first day anything was counted, at all
        QDate countingSince;
        int messages = 0;
        int minutes = 0;
        int mentions = 0;
        int activeDays = 0;
        int daysInRange = 0;
        int longestStreak = 0;
        QDate busiestDay;
        int busiestDayMessages = 0;
        std::array<int, 24> hours{};
        Ranking channels;
        Ranking watched;
        Ranking emotes;
        Ranking fans;
        Ranking talkedTo;

        bool empty() const
        {
            return this->messages == 0 && this->minutes == 0;
        }
    };

    Summary summarize(QDate from, QDate to) const;

    /// A link to the picture of an emote you used, or empty if unknown
    QString emoteUrl(const QString &name) const;

private:
    WrappedStats();

    struct Day {
        QHash<QString, int> messages;
        QHash<QString, int> minutes;
        QHash<QString, int> emotes;
        QHash<QString, int> mentionedBy;
        QHash<QString, int> talkedTo;
        std::array<int, 24> hours{};
        int mentions = 0;
    };

    Day &today();
    void countMinute();
    void load();
    void markDirty();
    void announceIfDue();

    std::map<QDate, Day> days_;
    QHash<QString, QString> emoteUrls_;
    QTimer announceTimer_;
    QDate since_;
    QTimer minuteTimer_;
    QTimer saveTimer_;
    bool dirty_ = false;
    bool started_ = false;
};

}  // namespace wrapped

}  // namespace chatterino
