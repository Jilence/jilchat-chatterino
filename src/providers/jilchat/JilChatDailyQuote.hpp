// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QDate>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringView>
#include <QTime>
#include <QTimer>

namespace chatterino {

/// A short opener for the quote that fits the time of day, never the same as
/// `previousLeadIn`.
QString dailyQuoteLeadIn(const QTime &time, QRandomGenerator &random,
                         QStringView previousLeadIn = {});
bool dailyQuoteIsDue(const QDate &lastShownDate, const QDate &today);

/// Shows a random quote from zenquotes.io once a day in the selected
/// chat, when the local day begins or the app starts later that day.
class JilChatDailyQuote final : public QObject
{
public:
    static JilChatDailyQuote &instance();

    void start();

private:
    JilChatDailyQuote();

    void showIfDue();
    void scheduleNextDay();
    /// Tries again later, e.g. when no chat is selected or zenquotes failed
    void scheduleRetry(int ms);

    QTimer timer_;
    bool started_ = false;
    bool loading_ = false;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
