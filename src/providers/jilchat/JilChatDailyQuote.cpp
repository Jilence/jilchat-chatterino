// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jilchat/JilChatDailyQuote.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

namespace {

using namespace chatterino;

/// A random quote each time, so everyone gets their own
constexpr auto QUOTE_URL = "https://zenquotes.io/api/random";
/// No chat selected yet
constexpr int RETRY_CHANNEL_MS = 30'000;
/// zenquotes didn't answer
constexpr int RETRY_NETWORK_MS = 5 * 60'000;

constexpr std::array<QStringView, 10> MORNING_LEAD_INS{
    u"Good morning",
    u"A morning thought",
    u"To start the day",
    u"Before you get going",
    u"First thought of the day",
    u"One for the morning",
    u"Starting today",
    u"Morning reading",
    u"For the day ahead",
    u"Before the day gets busy",
};

constexpr std::array<QStringView, 10> AFTERNOON_LEAD_INS{
    u"A note for today",        u"Today's thought",
    u"Worth a thought",         u"While you're here",
    u"One for the afternoon",   u"An afternoon reminder",
    u"A thought to take along", u"Something to keep in mind",
    u"For the rest of the day", u"A moment between things",
};

constexpr std::array<QStringView, 10> EVENING_LEAD_INS{
    u"Good evening",
    u"One for this evening",
    u"Before the day is done",
    u"An evening thought",
    u"As the day winds down",
    u"Something for tonight",
    u"A thought before you go",
    u"For the evening",
    u"Still time for a thought",
    u"A note to end the day",
};

constexpr std::array<QStringView, 10> NEW_DAY_LEAD_INS{
    u"A new day",
    u"Past midnight",
    u"One for the new day",
    u"For the hours ahead",
    u"The first thought today",
    u"For the late hours",
    u"A thought after midnight",
    u"Another day begins",
    u"If you're still awake",
    u"Midnight reading",
};

template <std::size_t Size>
QString chooseLeadIn(const std::array<QStringView, Size> &choices,
                     QRandomGenerator &random, QStringView previous)
{
    static_assert(Size > 1);
    const auto previousChoice =
        std::find(choices.begin(), choices.end(), previous);
    const bool hasPrevious = previousChoice != choices.end();
    auto index = static_cast<std::size_t>(
        random.bounded(static_cast<quint32>(Size - hasPrevious)));
    if (hasPrevious &&
        index >= static_cast<std::size_t>(previousChoice - choices.begin()))
    {
        ++index;
    }
    return choices.at(index).toString();
}

MessagePtr makeQuoteMessage(const QString &quote, const QString &leadIn)
{
    const auto text = QStringLiteral("· “%1”").arg(quote);
    const auto plainText = leadIn + u' ' + text;

    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::Centered);
    builder->flags.set(MessageFlag::DoNotLog);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder.emplace<TextElement>(
        leadIn,
        MessageElementFlags{MessageElementFlag::Text,
                            MessageElementFlag::AlwaysShow},
        MessageColor::Link, FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(
        text,
        MessageElementFlags{MessageElementFlag::Text,
                            MessageElementFlag::AlwaysShow},
        MessageColor::System);
    builder->messageText = plainText;
    builder->searchText = plainText;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    return builder.release();
}

/// The chat in the split last selected, if it is one with messages
ChannelPtr selectedChannel()
{
    auto *window = getApp()->getWindows()->getLastSelectedWindow();
    if (window == nullptr)
    {
        return nullptr;
    }
    auto *page = window->getNotebook().getSelectedPage();
    auto *split = page != nullptr ? page->getSelectedSplit() : nullptr;
    auto channel = split != nullptr ? split->getChannel() : nullptr;
    if (!channel || channel->isEmpty())
    {
        return nullptr;
    }

    switch (channel->getType())
    {
        case Channel::Type::Twitch:
        case Channel::Type::Kick:
        case Channel::Type::Multi:
        case Channel::Type::YouTube:
            return channel;
        default:
            return nullptr;
    }
}

}  // namespace

namespace chatterino {

QString dailyQuoteLeadIn(QTime time, QRandomGenerator &random,
                         QStringView previousLeadIn)
{
    const auto *leadIns = &NEW_DAY_LEAD_INS;
    if (time.hour() >= 5 && time.hour() < 12)
    {
        leadIns = &MORNING_LEAD_INS;
    }
    else if (time.hour() >= 12 && time.hour() < 18)
    {
        leadIns = &AFTERNOON_LEAD_INS;
    }
    else if (time.hour() >= 18)
    {
        leadIns = &EVENING_LEAD_INS;
    }

    return chooseLeadIn(*leadIns, random, previousLeadIn);
}

bool dailyQuoteIsDue(QDate lastShownDate, QDate today)
{
    return today.isValid() &&
           (!lastShownDate.isValid() || lastShownDate < today);
}

JilChatDailyQuote &JilChatDailyQuote::instance()
{
    static JilChatDailyQuote quote;
    return quote;
}

JilChatDailyQuote::JilChatDailyQuote()
{
    this->timer_.setSingleShot(true);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->showIfDue();
    });
}

void JilChatDailyQuote::start()
{
    if (this->started_)
    {
        return;
    }
    this->started_ = true;
    getSettings()->showDailyQuote.connect(
        [this](bool enabled) {
            this->timer_.stop();
            if (enabled)
            {
                QTimer::singleShot(0, this, [this] {
                    this->showIfDue();
                });
            }
        },
        this->signalHolder_, false);
    QTimer::singleShot(1500, this, [this] {
        this->showIfDue();
    });
}

void JilChatDailyQuote::showIfDue()
{
    if (!getSettings()->showDailyQuote || this->loading_)
    {
        return;
    }

    const auto today = QDate::currentDate();
    const auto lastDate = QDate::fromString(
        getSettings()->dailyQuoteLastDate.getValue(), Qt::ISODate);
    if (!dailyQuoteIsDue(lastDate, today))
    {
        this->scheduleNextDay();
        return;
    }

    if (!selectedChannel())
    {
        this->scheduleRetry(RETRY_CHANNEL_MS);
        return;
    }

    this->loading_ = true;
    NetworkRequest(QString::fromLatin1(QUOTE_URL))
        .timeout(15000)
        .onSuccess([this, today](const NetworkResult &result) {
            this->loading_ = false;
            // [{"q": quote, "a": author}]; only the quote is shown
            const auto list = result.parseJsonArray();
            const auto quote =
                list.isEmpty() ? QJsonObject{} : list.at(0).toObject();
            const auto body = quote.value("q").toString().trimmed();
            if (body.isEmpty())
            {
                this->scheduleRetry(RETRY_NETWORK_MS);
                return;
            }
            const auto channel = selectedChannel();
            if (!channel || !getSettings()->showDailyQuote)
            {
                this->scheduleRetry(RETRY_CHANNEL_MS);
                return;
            }

            auto &random = *QRandomGenerator::global();
            const auto leadIn = dailyQuoteLeadIn(
                QTime::currentTime(), random,
                getSettings()->dailyQuoteLastLeadIn.getValue());
            channel->addMessage(makeQuoteMessage(body, leadIn),
                                MessageContext::Original);
            getSettings()->dailyQuoteLastLeadIn = leadIn;
            getSettings()->dailyQuoteLastDate = today.toString(Qt::ISODate);
            // Saved right away, so a crash doesn't show it twice
            std::ignore = getSettings()->requestSave();
            this->scheduleNextDay();
        })
        .onError([this](const NetworkResult &) {
            this->loading_ = false;
            this->scheduleRetry(RETRY_NETWORK_MS);
        })
        .execute();
}

void JilChatDailyQuote::scheduleNextDay()
{
    const auto now = QDateTime::currentDateTime();
    const auto nextDay =
        QDateTime(now.date().addDays(1), QTime(0, 0)).addSecs(1);
    const auto delay = std::clamp<qint64>(
        now.msecsTo(nextDay), 1000,
        static_cast<qint64>(std::numeric_limits<int>::max()));
    this->timer_.start(static_cast<int>(delay));
}

void JilChatDailyQuote::scheduleRetry(int ms)
{
    this->timer_.start(ms);
}

}  // namespace chatterino
