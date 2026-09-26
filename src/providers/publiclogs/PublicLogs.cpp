// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/publiclogs/PublicLogs.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "util/IrcHelpers.hpp"
#include "util/VectorMessageSink.hpp"

#include <IrcMessage>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocale>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>

namespace chatterino::publiclogs {

using namespace literals;

namespace {

const QString BASE_URL = u"https://logs.zonian.dev"_s;

QString encode(const QString &part)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(part));
}

QUrl makeUrl(const QString &path,
             std::initializer_list<std::pair<QString, QString>> items)
{
    QUrl url(BASE_URL + path);
    QUrlQuery query;
    for (const auto &[key, value] : items)
    {
        query.addQueryItem(key, value);
    }
    url.setQuery(query);
    return url;
}

// Measured on logs.zonian.dev: about 10 requests at once, then about one per
// second. Stay a bit below so we don't run into HTTP 429. Up to this many
// downloads run in parallel, since the limit counts requests, not data.
constexpr int MAX_PARALLEL = 8;
constexpr double BURST = 8.0;
constexpr double PER_SECOND = 1.0;
constexpr int MAX_RETRIES = 5;

struct Request {
    QUrl url;
    int timeoutMs = 0;
    std::function<void(const NetworkResult &)> onSuccess;
    std::function<void(const NetworkResult &)> onError;
    std::function<bool()> stillWanted;
    int attempt = 0;

    bool wanted() const
    {
        return !this->stillWanted || this->stillWanted();
    }
};

/// Sends the requests of the whole app, paced like the service's rate limit.
class RequestQueue : public QObject
{
public:
    explicit RequestQueue(QObject *parent)
        : QObject(parent)
    {
        this->pumpTimer_.setSingleShot(true);
        QObject::connect(&this->pumpTimer_, &QTimer::timeout, this,
                         &RequestQueue::pump);
        this->clock_.start();
    }

    void add(Request request)
    {
        this->queue_.push_back(std::move(request));
        this->pump();
    }

private:
    void pump()
    {
        const auto elapsedSeconds =
            static_cast<double>(this->clock_.restart()) / 1000.0;
        this->tokens_ =
            std::min(BURST, this->tokens_ + (elapsedSeconds * PER_SECOND));

        while (this->active_ < MAX_PARALLEL && !this->queue_.empty())
        {
            if (!this->queue_.front().wanted())
            {
                this->queue_.pop_front();
                continue;
            }
            if (this->tokens_ < 1.0)
            {
                break;
            }
            this->tokens_ -= 1.0;
            ++this->active_;
            this->send(
                std::make_shared<Request>(std::move(this->queue_.front())));
            this->queue_.pop_front();
        }

        // Out of tokens: come back when the next one is available.
        if (!this->queue_.empty() && this->active_ < MAX_PARALLEL &&
            !this->pumpTimer_.isActive())
        {
            const auto waitMs = static_cast<int>(
                std::ceil((1.0 - this->tokens_) / PER_SECOND * 1000.0));
            this->pumpTimer_.start(std::max(waitMs, 50));
        }
    }

    void finish(const std::shared_ptr<Request> &request,
                const NetworkResult &result, bool success)
    {
        --this->active_;
        const auto &callback = success ? request->onSuccess : request->onError;
        if (callback && request->wanted())
        {
            callback(result);
        }
        this->pump();
    }

    void send(std::shared_ptr<Request> request)
    {
        const QPointer<RequestQueue> self(this);
        NetworkRequest(request->url)
            .timeout(request->timeoutMs)
            .onSuccess([self, request](const NetworkResult &result) {
                if (self)
                {
                    self->finish(request, result, true);
                }
            })
            .onError([self, request](const NetworkResult &result) {
                if (!self)
                {
                    return;
                }
                // Rate limited: wait and try again, keeping the slot so the
                // other requests wait as well.
                if (result.status() == 429 && request->attempt < MAX_RETRIES &&
                    request->wanted())
                {
                    ++request->attempt;
                    QTimer::singleShot(1000 * (1 << request->attempt),
                                       self.data(), [self, request] {
                                           self->send(request);
                                       });
                    return;
                }
                self->finish(request, result, false);
            })
            .execute();
    }

    std::deque<Request> queue_;
    int active_ = 0;
    double tokens_ = BURST;
    QElapsedTimer clock_;
    QTimer pumpTimer_;
};

RequestQueue &requestQueue()
{
    // Owned by the application, so it goes away with it.
    static auto *queue = new RequestQueue(QCoreApplication::instance());
    return *queue;
}

}  // namespace

QDate LogDate::firstDay() const
{
    return {this->year, this->month, this->day > 0 ? this->day : 1};
}

QDate LogDate::lastDay() const
{
    const auto first = this->firstDay();
    return this->day > 0 ? first : first.addMonths(1).addDays(-1);
}

QUrl listUrl(const QString &channel, const QString &user)
{
    if (user.isEmpty())
    {
        return makeUrl(u"/list"_s, {{u"channel"_s, channel}});
    }
    return makeUrl(u"/list"_s, {{u"channel"_s, channel}, {u"user"_s, user}});
}

QUrl userMonthUrl(const QString &channel, const QString &user,
                  const LogDate &month, int limit, int offset)
{
    const auto path = u"/channel/%1/user/%2/%3/%4"_s.arg(
        encode(channel), encode(user), QString::number(month.year),
        QString::number(month.month));
    if (limit <= 0)
    {
        return makeUrl(path, {{u"json"_s, u"1"_s}});
    }
    return makeUrl(path, {
                             {u"json"_s, u"1"_s},
                             {u"reverse"_s, u"1"_s},
                             {u"limit"_s, QString::number(limit)},
                             {u"offset"_s, QString::number(offset)},
                         });
}

QUrl channelDayUrl(const QString &channel, const LogDate &day)
{
    return makeUrl(u"/channel/%1/%2/%3/%4"_s.arg(
                       encode(channel), QString::number(day.year),
                       QString::number(day.month), QString::number(day.day)),
                   {{u"json"_s, u"1"_s}});
}

QUrl channelRangeUrl(const QString &channel, const QDateTime &from,
                     const QDateTime &to, int limit)
{
    return makeUrl(u"/channel/%1"_s.arg(encode(channel)),
                   {
                       {u"json"_s, u"1"_s},
                       {u"reverse"_s, u"1"_s},
                       {u"limit"_s, QString::number(limit)},
                       {u"from"_s, from.toString(Qt::ISODateWithMs)},
                       {u"to"_s, to.toString(Qt::ISODateWithMs)},
                   });
}

std::vector<LogDate> parseLogDates(const NetworkResult &result)
{
    std::vector<LogDate> dates;
    for (const auto &value :
         result.parseJson().value("availableLogs").toArray())
    {
        const auto obj = value.toObject();
        const LogDate date{
            .year = obj.value("year").toString().toInt(),
            .month = obj.value("month").toString().toInt(),
            .day = obj.value("day").toString().toInt(),
        };
        if (date.firstDay().isValid())
        {
            dates.push_back(date);
        }
    }
    std::ranges::sort(dates, std::greater{});
    return dates;
}

QJsonArray parseLogLines(const NetworkResult &result)
{
    return result.parseJson().value("messages").toArray();
}

std::vector<MessagePtr> buildMessages(
    const QJsonArray &lines, TwitchChannel *channel, bool newestFirst,
    const std::function<bool(const QJsonObject &)> &accept)
{
    VectorMessageSink sink({}, MessageFlag::RecentMessage);
    const auto build = [&](const QJsonValue &value) {
        const auto line = value.toObject();
        if (accept && !accept(line))
        {
            return;
        }
        auto *ircMessage = Communi::IrcMessage::fromData(
            markIrcLineHistorical(line.value("raw").toString()).toUtf8(),
            nullptr);
        if (ircMessage == nullptr)
        {
            return;
        }
        IrcMessageHandler::parseMessageInto(ircMessage, sink, channel);
        ircMessage->deleteLater();
    };

    if (newestFirst)
    {
        for (auto i = lines.size(); i-- > 0;)
        {
            build(lines.at(i));
        }
    }
    else
    {
        for (const auto &value : lines)
        {
            build(value);
        }
    }
    return std::move(sink).takeMessages();
}

MessagePtr makeDaySeparator(const QDate &day)
{
    return makeSystemMessage(QLocale().toString(day, QLocale::LongFormat),
                             QTime(0, 0));
}

void get(const QUrl &url, int timeoutMs,
         std::function<void(const NetworkResult &)> onSuccess,
         std::function<void(const NetworkResult &)> onError,
         std::function<bool()> stillWanted)
{
    requestQueue().add({
        .url = url,
        .timeoutMs = timeoutMs,
        .onSuccess = std::move(onSuccess),
        .onError = std::move(onError),
        .stillWanted = std::move(stillWanted),
    });
}

}  // namespace chatterino::publiclogs
