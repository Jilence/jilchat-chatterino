// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <boost/unordered/unordered_flat_map.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;
class QJsonObject;

namespace chatterino {

struct Emote;

class LimerinoBadges final : public QObject
{
public:
    LimerinoBadges();
    ~LimerinoBadges() override;

    void initialize();
    std::vector<std::shared_ptr<const Emote>> getBadges(
        const UserId &userID) const;

    /// Fetches the badges now. `onDone` gets whether that worked, in the
    /// GUI thread.
    void reload(std::function<void(bool)> onDone);

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    bool applyPayload(const QByteArray &payload);
    void applyHolders(const QJsonObject &event);

    void start();
    void stop();
    void refresh();
    void schedule(std::chrono::milliseconds delay);
    void noteSuccess();
    void noteFailure(bool retry);
    void finishCallbacks(bool success);
    /// Draws from the copy kept from the last run until the first refresh,
    /// which then only has to ask whether it is still current
    void loadStoredCopy();
    void storeCopy(const QByteArray &payload, const QByteArray &etag) const;
    void catchUp();

    void listen();
    void closeEventStream();
    void readEventStream();
    void onEventFinished();
    void parseEvents();
    void dispatchEvent();
    void handleHolders(const QJsonObject &event);

    mutable std::shared_mutex mutex_;
    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>> catalog_;
    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>> users_;

    QByteArray etag_;
    QTimer refreshTimer_;
    std::chrono::steady_clock::time_point nextAt_{};
    std::chrono::milliseconds pendingDelay_{};
    int failures_ = 0;
    bool requestInFlight_ = false;
    bool storedCopyLoaded_ = false;
    /// Waiting for the request in flight
    std::vector<std::function<void(bool)>> pendingCallbacks_;
    bool again_ = false;
    bool canPatch_ = false;
    bool animate_ = true;
    bool streamClosed_ = true;

    QNetworkAccessManager *network_ = nullptr;
    QNetworkReply *eventReply_ = nullptr;
    QByteArray eventBuffer_;
    QByteArray eventType_;
    QByteArray eventData_;
    int eventRetryMs_ = 60 * 1000;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
