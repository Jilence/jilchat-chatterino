#pragma once

#include "controllers/completion/CommandUsage.hpp"

#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

struct PotatCommand {
    QString name;
    QString usage;
    bool dynamicUsage = true;
    bool alias = false;
    std::shared_ptr<const completion::CommandUsage> argumentHint;

    bool operator==(const PotatCommand &other) const
    {
        return this->name == other.name && this->usage == other.usage &&
               this->dynamicUsage == other.dynamicUsage &&
               this->alias == other.alias &&
               (this->argumentHint == other.argumentHint ||
                (this->argumentHint && other.argumentHint &&
                 *this->argumentHint == *other.argumentHint));
    }
};

// A channel custom command. An empty name means `prefix` is the whole trigger.
struct PotatChannelCommand {
    QString prefix;
    QString name;
    QString usage;

    // A channel prefix leaves the command name beside it. Anything else, such
    // as "paul" or "!subs", keeps the whole trigger in prefix.
    bool usesChannelPrefix() const
    {
        return !this->name.isEmpty();
    }
};

struct PotatChannelRecord {
    bool joined = false;
    QStringList prefixes;
    std::vector<PotatChannelCommand> commands;
};

namespace potat::detail {

std::vector<PotatCommand> parseCommands(const QByteArray &payload);
std::optional<PotatChannelRecord> parseChannel(const QByteArray &payload);

}  // namespace potat::detail

class PotatCommands final : public QObject
{
public:
    PotatCommands() = default;

    void ensureLoaded();
    void ensureChannel(const QString &channelLogin);
    const std::vector<PotatCommand> &commands() const;
    bool isLoading() const;
    bool channelReady(const QString &channelLogin) const;
    bool isChannelLoading(const QString &channelLogin) const;
    bool isJoined(const QString &channelLogin) const;
    // True when this token should open Potat suggestions for the channel.
    bool suggestionQuery(const QString &channelLogin,
                         const QString &query) const;
    QStringList prefixesFor(const QString &channelLogin) const;
    const std::vector<PotatChannelCommand> &channelCommands(
        const QString &channelLogin) const;

    pajlada::Signals::NoArgSignal commandsUpdated;

private:
    struct ChannelState {
        QString etag;
        bool cacheChecked = false;
        bool ready = false;
        bool requestInFlight = false;
        bool fetchedThisSession = false;
        bool retryWithoutEtag = false;
        int requestAttempts = 0;
        QDateTime nextRetryAt;
        PotatChannelRecord record;
    };

    void loadCache();
    void requestCatalog(bool useEtag = true);
    bool saveCache(const QByteArray &payload) const;
    void saveMetadata() const;
    void loadChannelCache(const QString &login);
    void requestChannel(const QString &login, bool useEtag);
    bool saveChannelCache(const QString &login,
                          const QByteArray &payload) const;
    void saveChannelMetadata(const QString &login) const;
    ChannelState *channelState(const QString &channelLogin);
    const ChannelState *channelState(const QString &channelLogin) const;
    static QString normalizeLogin(const QString &channelLogin);

    std::vector<PotatCommand> commands_;
    QString etag_;
    bool cacheChecked_ = false;
    bool requestInFlight_ = false;
    bool fetchedThisSession_ = false;
    bool retryWithoutEtag_ = false;
    int requestAttempts_ = 0;
    QDateTime nextRetryAt_;
    QHash<QString, ChannelState> channelStates_;
};

}  // namespace chatterino
