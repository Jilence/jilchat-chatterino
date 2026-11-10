// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QByteArray>
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

/// Badges from Solrino, a Moltorino-based client: the badge each person
/// chose to wear in Solrino, read from Solrino's public badge list.
class SolrinoBadges final : public QObject
{
public:
    SolrinoBadges();

    std::optional<EmotePtr> getBadge(const QString &userID) const;

    /// Loads the badges, replacing the ones loaded before.
    ///
    /// @param onDone called once the answer is there, with whether loading
    ///               worked
    void load(const std::function<void(bool)> &onDone = {});

private:
    bool applyPayload(const QByteArray &payload);

    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, EmotePtr> users_;
    QByteArray lastPayload_;
    QTimer refreshTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
