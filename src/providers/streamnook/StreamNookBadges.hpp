// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QHashFunctions>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// The badge a StreamNook member wears, from the public badge file of
/// https://docs.streamnook.app/api-cosmetics
class StreamNookBadges final : public QObject
{
public:
    StreamNookBadges();

    /// The one badge StreamNook draws for this Twitch user id
    std::optional<EmotePtr> getBadge(const QString &userID) const;

    /// Loads the badges, replacing the ones loaded before.
    ///
    /// @param onDone called once the answer is there, with whether loading
    ///               worked; from any thread
    void load(const std::function<void(bool)> &onDone = {});

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, EmotePtr> badgeMap_;
    QTimer refreshTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
