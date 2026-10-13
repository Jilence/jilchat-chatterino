// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Impl.hpp"

#include "Application.hpp"
#include "common/Env.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/VectorMessageSink.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSet>
#include <QUrlQuery>

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>

namespace chatterino::recentmessages::detail {

std::vector<Communi::IrcMessage *> parseRecentMessages(
    const QJsonObject &jsonRoot)
{
    const auto jsonMessages = jsonRoot.value("messages").toArray();
    std::vector<Communi::IrcMessage *> messages;

    if (jsonMessages.empty())
    {
        return messages;
    }

    for (const auto &jsonMessage : jsonMessages)
    {
        auto content = unescapeZeroWidthJoiner(jsonMessage.toString());

        auto *message =
            Communi::IrcMessage::fromData(content.toUtf8(), nullptr);

        messages.emplace_back(message);
    }

    return messages;
}

std::vector<MessagePtr> buildRecentMessages(
    std::vector<Communi::IrcMessage *> &messages, Channel *channel)
{
    VectorMessageSink sink({}, MessageFlag::RecentMessage);

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel);
    if (!twitchChannel)
    {
        return {};
    }

    for (auto *message : messages)
    {
        if (auto optReceivedTs = message->tags().get("rm-received-ts"))
        {
            const auto msgDate =
                QDateTime::fromMSecsSinceEpoch(optReceivedTs->toLongLong())
                    .date();

            if (msgDate != channel->lastDate_)
            {
                channel->lastDate_ = msgDate;
                auto msg = makeSystemMessage(
                    QLocale().toString(msgDate, QLocale::LongFormat),
                    QTime(0, 0));
                sink.addMessage(msg, MessageContext::Original);
            }
        }

        IrcMessageHandler::parseMessageInto(message, sink, twitchChannel);

        message->deleteLater();
    }

    return std::move(sink).takeMessages();
}

namespace {

/// How much of a history is built in one go, whichever comes first
constexpr size_t BUILD_SLICE_MESSAGE_LIMIT = 25;
constexpr qint64 BUILD_SLICE_TIME_LIMIT_MS = 3;
/// Building this many messages leaves enough unused memory behind to ask for
/// it to be given back
constexpr size_t MEMORY_RELIEF_MESSAGE_THRESHOLD = 128;

/// A history that is being built
class RecentMessagesBuildTask
{
public:
    RecentMessagesBuildTask(
        QJsonArray messages, std::weak_ptr<Channel> channel,
        std::function<void(std::vector<MessagePtr>)> onBuilt)
        : messages_(std::move(messages))
        , channel_(std::move(channel))
        , onBuilt_(std::move(onBuilt))
        , sink_({}, MessageFlag::RecentMessage)
    {
    }

    /// Builds some of the messages.
    ///
    /// @returns whether the task is done
    bool processSlice()
    {
        auto channel = this->channel_.lock();
        auto *twitchChannel =
            channel ? dynamic_cast<TwitchChannel *>(channel.get()) : nullptr;
        if (twitchChannel == nullptr || isAppAboutToQuit())
        {
            return true;
        }

        QElapsedTimer timer;
        timer.start();
        size_t processed = 0;
        while (this->nextMessage_ < this->messages_.size())
        {
            const auto content = unescapeZeroWidthJoiner(
                this->messages_.at(this->nextMessage_++).toString());
            // the same as buildRecentMessages does with each message
            auto *message =
                Communi::IrcMessage::fromData(content.toUtf8(), nullptr);
            if (message != nullptr)
            {
                if (auto optReceivedTs = message->tags().get("rm-received-ts"))
                {
                    const auto msgDate = QDateTime::fromMSecsSinceEpoch(
                                             optReceivedTs->toLongLong())
                                             .date();

                    if (msgDate != channel->lastDate_)
                    {
                        channel->lastDate_ = msgDate;
                        auto msg = makeSystemMessage(
                            QLocale().toString(msgDate, QLocale::LongFormat),
                            QTime(0, 0));
                        this->sink_.addMessage(msg, MessageContext::Original);
                    }
                }

                IrcMessageHandler::parseMessageInto(message, this->sink_,
                                                    twitchChannel);
                message->deleteLater();
                ++this->parsedMessageCount_;
            }

            ++processed;
            if (processed >= BUILD_SLICE_MESSAGE_LIMIT ||
                timer.elapsed() >= BUILD_SLICE_TIME_LIMIT_MS)
            {
                break;
            }
        }

        if (this->nextMessage_ != this->messages_.size())
        {
            return false;
        }

        auto onBuilt = std::move(this->onBuilt_);
        onBuilt(std::move(this->sink_).takeMessages());
        return true;
    }

    size_t messageCount() const
    {
        return this->parsedMessageCount_;
    }

private:
    QJsonArray messages_;
    std::weak_ptr<Channel> channel_;
    std::function<void(std::vector<MessagePtr>)> onBuilt_;
    VectorMessageSink sink_;
    qsizetype nextMessage_ = 0;
    size_t parsedMessageCount_ = 0;
};

/// The histories waiting to be built, the first one being worked on
struct BuildSchedulerState {
    std::deque<std::shared_ptr<RecentMessagesBuildTask>> tasks;
    bool callbackQueued = false;
    /// How many messages were built since there last was nothing to do
    size_t completedMessagesSinceDrain = 0;
};

BuildSchedulerState &buildSchedulerState()
{
    // lives as long as the application: there may be a slice left to run
    // while it shuts down
    static auto *state = new BuildSchedulerState;
    return *state;
}

void scheduleNextBuildSlice();

void processNextBuildSlice()
{
    auto &state = buildSchedulerState();
    state.callbackQueued = false;

    if (isAppAboutToQuit())
    {
        state.tasks.clear();
        return;
    }
    if (state.tasks.empty())
    {
        return;
    }

    auto task = std::move(state.tasks.front());
    state.tasks.pop_front();
    if (!task->processSlice())
    {
        // back in line, so that all histories make progress
        state.tasks.push_back(std::move(task));
    }
    else
    {
        state.completedMessagesSinceDrain += task->messageCount();
        task.reset();
    }

    if (state.tasks.empty())
    {
        // Parsing leaves a lot of freed memory behind. Once all histories
        // are done, ask for it to go back to the system.
        const auto completedMessages = state.completedMessagesSinceDrain;
        state.completedMessagesSinceDrain = 0;
        if (completedMessages >= MEMORY_RELIEF_MESSAGE_THRESHOLD)
        {
            requestMemoryPressureRelief();
        }
    }

    scheduleNextBuildSlice();
}

void scheduleNextBuildSlice()
{
    auto &state = buildSchedulerState();
    if (state.callbackQueued || state.tasks.empty())
    {
        return;
    }

    // queued: whatever else waits for the GUI thread comes first
    state.callbackQueued = true;
    QMetaObject::invokeMethod(qApp, processNextBuildSlice,
                              Qt::QueuedConnection);
}

/// Two services seldom receive a message at the very same time
constexpr qint64 RECEIPT_DUPLICATE_TOLERANCE_MS = 2000;

struct MergeCandidate {
    QString raw;
    QString key;
    qint64 timestamp{};
    bool hasStableIdentity{};
};

struct ReceiptOccurrence {
    qint64 timestamp{};
    size_t batch{};
};

std::optional<MergeCandidate> inspectMessage(const QString &raw)
{
    const auto content = unescapeZeroWidthJoiner(raw);
    auto *message = Communi::IrcMessage::fromData(content.toUtf8(), nullptr);
    if (message == nullptr)
    {
        return std::nullopt;
    }

    // a copy: two tags are taken out of it below
    auto tags = message->tags().raw();
    bool hasSentTimestamp = false;
    auto timestamp =
        tags.value(QStringLiteral("tmi-sent-ts")).toLongLong(&hasSentTimestamp);
    bool hasReceiptTimestamp = false;
    if (!hasSentTimestamp)
    {
        timestamp = tags.value(QStringLiteral("rm-received-ts"))
                        .toLongLong(&hasReceiptTimestamp);
    }
    if (!hasSentTimestamp && !hasReceiptTimestamp)
    {
        timestamp = 0;
    }

    QString key;
    const auto messageId = tags.value(QStringLiteral("id")).toString();
    if (!messageId.isEmpty())
    {
        key = QStringLiteral("id:") + messageId;
    }
    else
    {
        tags.remove(QStringLiteral("rm-received-ts"));
        tags.remove(QStringLiteral("historical"));

        QJsonObject canonical;
        canonical.insert(QStringLiteral("prefix"), message->prefix());
        canonical.insert(QStringLiteral("command"), message->command());
        canonical.insert(QStringLiteral("tags"),
                         QJsonObject::fromVariantMap(tags));

        QJsonArray parameters;
        const auto ircParameters = message->parameters();
        for (const auto &parameter : ircParameters)
        {
            parameters.append(parameter);
        }
        canonical.insert(QStringLiteral("parameters"), parameters);
        key = QString::fromUtf8(
            QJsonDocument(canonical).toJson(QJsonDocument::Compact));
    }

    delete message;
    return MergeCandidate{
        .raw = raw,
        .key = std::move(key),
        .timestamp = timestamp,
        .hasStableIdentity = !messageId.isEmpty() || hasSentTimestamp,
    };
}

bool closeReceiptTimes(qint64 lhs, qint64 rhs)
{
    const auto delta =
        lhs > rhs ? quint64(lhs) - quint64(rhs) : quint64(rhs) - quint64(lhs);
    return delta <= RECEIPT_DUPLICATE_TOLERANCE_MS;
}

QString urlTemplateOf(RecentMessagesApi api)
{
    switch (api)
    {
        case RecentMessagesApi::Zneix:
            return QStringLiteral("https://recent-messages.zneix.eu/api/v2/"
                                  "recent-messages/%1");

        case RecentMessagesApi::Lilb:
            return QStringLiteral(
                "https://rm.lilb.dev/api/v2/recent-messages/%1");

        case RecentMessagesApi::Zonian:
            return QStringLiteral("https://logs.zonian.dev/rm/%1");

        case RecentMessagesApi::Robotty:
        default:
            return QStringLiteral("https://recent-messages.robotty.de/api/v2/"
                                  "recent-messages/%1");
    }
}

}  // namespace

QJsonArray mergeRecentMessageBatches(const std::vector<QJsonArray> &batches,
                                     const int limit)
{
    std::vector<MergeCandidate> candidates;
    QSet<QString> stableIdentities;
    QHash<QString, std::vector<ReceiptOccurrence>> receiptOccurrences;
    QHash<QString, size_t> untimedRawMessages;

    for (size_t batchIndex = 0; batchIndex < batches.size(); ++batchIndex)
    {
        const auto &batch = batches[batchIndex];
        candidates.reserve(candidates.size() + size_t(batch.size()));
        for (const auto &value : batch)
        {
            if (!value.isString())
            {
                continue;
            }

            auto candidate = inspectMessage(value.toString());
            if (!candidate)
            {
                continue;
            }

            bool isDuplicate = false;
            if (candidate->hasStableIdentity)
            {
                isDuplicate = stableIdentities.contains(candidate->key);
                stableIdentities.insert(candidate->key);
            }
            else if (candidate->timestamp > 0)
            {
                auto &occurrences = receiptOccurrences[candidate->key];
                isDuplicate = std::ranges::any_of(
                    occurrences, [candidateTimestamp = candidate->timestamp,
                                  batchIndex](const auto &occurrence) {
                        return occurrence.batch != batchIndex &&
                               closeReceiptTimes(occurrence.timestamp,
                                                 candidateTimestamp);
                    });
                if (!isDuplicate)
                {
                    occurrences.push_back({candidate->timestamp, batchIndex});
                }
            }
            else
            {
                const auto previous =
                    untimedRawMessages.constFind(candidate->raw);
                isDuplicate = previous != untimedRawMessages.cend() &&
                              *previous != batchIndex;
                if (previous == untimedRawMessages.cend())
                {
                    untimedRawMessages.insert(candidate->raw, batchIndex);
                }
            }

            if (isDuplicate)
            {
                continue;
            }

            candidates.emplace_back(std::move(*candidate));
        }
    }

    std::ranges::stable_sort(candidates, [](const auto &lhs, const auto &rhs) {
        const auto lhsTimestamp = lhs.timestamp > 0
                                      ? lhs.timestamp
                                      : std::numeric_limits<qint64>::max();
        const auto rhsTimestamp = rhs.timestamp > 0
                                      ? rhs.timestamp
                                      : std::numeric_limits<qint64>::max();
        return lhsTimestamp < rhsTimestamp;
    });

    if (limit > 0 && candidates.size() > size_t(limit))
    {
        candidates.erase(candidates.begin(), candidates.end() - limit);
    }

    QJsonArray merged;
    for (const auto &candidate : candidates)
    {
        merged.append(candidate.raw);
    }
    return merged;
}

void buildRecentMessagesBatched(
    QJsonArray messages, std::weak_ptr<Channel> channel,
    std::function<void(std::vector<MessagePtr>)> onBuilt)
{
    assertInGuiThread();

    buildSchedulerState().tasks.emplace_back(
        std::make_shared<RecentMessagesBuildTask>(
            std::move(messages), std::move(channel), std::move(onBuilt)));
    scheduleNextBuildSlice();
}

QString recentMessagesApiUrlTemplate()
{
    if (!qEnvironmentVariable("CHATTERINO2_RECENT_MESSAGES_URL").isEmpty())
    {
        return Env::get().recentMessagesApiUrl;
    }

    return urlTemplateOf(getSettings()->recentMessagesApi.getEnum());
}

std::vector<QString> recentMessagesApiUrlTemplates()
{
    if (!qEnvironmentVariable("CHATTERINO2_RECENT_MESSAGES_URL").isEmpty())
    {
        // someone asked for this one service
        return {Env::get().recentMessagesApiUrl};
    }

    std::vector<QString> templates;
    templates.reserve(4);
    templates.push_back(recentMessagesApiUrlTemplate());
    for (const auto api : {RecentMessagesApi::Robotty, RecentMessagesApi::Zneix,
                           RecentMessagesApi::Lilb, RecentMessagesApi::Zonian})
    {
        auto urlTemplate = urlTemplateOf(api);
        if (std::ranges::find(templates, urlTemplate) == templates.end())
        {
            templates.push_back(std::move(urlTemplate));
        }
    }
    return templates;
}

QUrl constructRecentMessagesUrl(
    const QString &name, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before)
{
    return constructRecentMessagesUrl(recentMessagesApiUrlTemplate(), name,
                                      limit, after, before);
}

QUrl constructRecentMessagesUrl(
    const QString &urlTemplate, const QString &name, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before)
{
    QUrl url(urlTemplate.arg(name));
    QUrlQuery urlQuery(url);
    if (!urlQuery.hasQueryItem("limit"))
    {
        urlQuery.addQueryItem("limit", QString::number(limit));
    }
    if (after.has_value())
    {
        urlQuery.addQueryItem(
            "after", QString::number(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             after->time_since_epoch())
                             .count()));
    }
    if (before.has_value())
    {
        urlQuery.addQueryItem(
            "before", QString::number(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              before->time_since_epoch())
                              .count()));
    }
    url.setQuery(urlQuery);
    return url;
}

}  // namespace chatterino::recentmessages::detail
