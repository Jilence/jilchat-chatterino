// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Api.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/recentmessages/Impl.hpp"
#include "util/PostToThread.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace {

const auto &LOG = chatterinoRecentMessages;

}

namespace chatterino::recentmessages {

using namespace recentmessages::detail;

namespace {

/// How long one service gets to answer before the next one is asked
constexpr int SERVICE_TIMEOUT_MS = 5000;

/// Loads the recent messages of a channel, asking one service after the
/// other until one has them.
class LoadRequest : public std::enable_shared_from_this<LoadRequest>
{
public:
    QString channelName;
    std::weak_ptr<Channel> channelPtr;
    ResultCallback onLoaded;
    ErrorCallback onError;
    int limit{};
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after;
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before;
    /// The services, the preferred one first
    std::vector<QString> urlTemplates;

    void tryNextService()
    {
        if (isAppAboutToQuit() || this->channelPtr.expired())
        {
            return;
        }

        if (this->nextService_ >= this->urlTemplates.size())
        {
            if (!this->partialHistories_.empty())
            {
                // none has all of it: take what they have together
                this->finishMerged(true);
            }
            else
            {
                this->fail();
            }
            return;
        }

        const auto url = constructRecentMessagesUrl(
            this->urlTemplates[this->nextService_++], this->channelName,
            this->limit, this->after, this->before);
        auto self = this->shared_from_this();

        qCDebug(LOG) << "Asking" << url.host() << "for the recent messages of"
                     << this->channelName;

        NetworkRequest(url)
            .timeout(SERVICE_TIMEOUT_MS)
            .onSuccess([self, host = url.host()](const NetworkResult &result) {
                self->handleAnswer(host, result.parseJson());
            })
            .onError([self, host = url.host()](const NetworkResult &result) {
                self->lastError_ = result.formatError();
                qCDebug(LOG) << host << "failed for" << self->channelName << ':'
                             << self->lastError_;
                self->tryNextService();
            })
            .execute();
    }

private:
    void handleAnswer(const QString &host, const QJsonObject &root)
    {
        if (isAppAboutToQuit() || this->channelPtr.expired())
        {
            return;
        }

        const auto messages = root.value("messages");
        if (!messages.isArray())
        {
            this->lastError_ = QStringLiteral("invalid response");
            qCDebug(LOG) << host << "gave an invalid response for"
                         << this->channelName;
            this->tryNextService();
            return;
        }

        const auto errorCode = root.value("error_code").toString();
        if (errorCode == "channel_not_joined")
        {
            // The service only just started to follow the channel: it has
            // nothing, or not everything. Another one may have it all.
            this->lastError_ = errorCode;
            if (!messages.toArray().isEmpty())
            {
                this->partialHistories_.push_back(messages.toArray());
            }
            qCDebug(LOG) << host << "is not tracking" << this->channelName
                         << "yet";
            this->tryNextService();
            return;
        }
        if (!errorCode.isEmpty())
        {
            this->lastError_ = errorCode;
            qCDebug(LOG) << host << "gave an error for" << this->channelName
                         << ':' << errorCode;
            this->tryNextService();
            return;
        }

        qCDebug(LOG) << "Loaded the recent messages of" << this->channelName
                     << "from" << host;
        if (this->partialHistories_.empty())
        {
            this->finish(root, false);
            return;
        }

        // The others may know of messages from before this one kept track
        this->partialHistories_.push_back(messages.toArray());
        this->finishMerged(false);
    }

    void finishMerged(bool partial)
    {
        auto merged =
            mergeRecentMessageBatches(this->partialHistories_, this->limit);
        this->partialHistories_.clear();
        this->finish(QJsonObject{{"messages", merged}}, partial);
    }

    void finish(const QJsonObject &root, bool partial) const
    {
        // The messages are built on the GUI thread, a few at a time.
        postToThread([channelPtr = this->channelPtr,
                      messages = root.value("messages").toArray(),
                      onLoaded = this->onLoaded, partial]() mutable {
            if (isAppAboutToQuit())
            {
                return;
            }

            buildRecentMessagesBatched(
                std::move(messages), channelPtr,
                [channelPtr, onLoaded = std::move(onLoaded),
                 partial](const std::vector<MessagePtr> &built) {
                    auto shared = channelPtr.lock();
                    if (!shared || isAppAboutToQuit())
                    {
                        return;
                    }

                    if (partial && !built.empty())
                    {
                        shared->addSystemMessage(
                            "Message history service recovering, there may "
                            "be gaps in the message history.");
                    }

                    onLoaded(built);
                });
        });
    }

    void fail()
    {
        postToThread([channelPtr = this->channelPtr, onError = this->onError,
                      services = this->urlTemplates.size(),
                      lastError = this->lastError_] {
            auto shared = channelPtr.lock();
            if (!shared || isAppAboutToQuit())
            {
                return;
            }

            shared->addSystemMessage(
                (services > 1
                     ? QStringLiteral(
                           "Message history services unavailable (Error: %1)")
                     : QStringLiteral(
                           "Message history service unavailable (Error: %1)"))
                    .arg(lastError.isEmpty()
                             ? QStringLiteral("no service answered")
                             : lastError));
            onError();
        });
    }

    size_t nextService_{};
    /// What the services that don't have everything have
    std::vector<QJsonArray> partialHistories_;
    QString lastError_;
};

}  // namespace

void load(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before,
    const bool jitter)
{
    qCDebug(LOG) << "Loading recent messages for" << channelName;

    const long delayMs = jitter ? std::rand() % 100 : 0;
    QTimer::singleShot(delayMs, [=] {
        if (isAppAboutToQuit())
        {
            return;
        }

        auto request = std::make_shared<LoadRequest>();
        request->channelName = channelName;
        request->channelPtr = channelPtr;
        request->onLoaded = onLoaded;
        request->onError = onError;
        request->limit = limit;
        request->after = after;
        request->before = before;
        request->urlTemplates = recentMessagesApiUrlTemplates();
        request->tryNextService();
    });
}

}  // namespace chatterino::recentmessages
