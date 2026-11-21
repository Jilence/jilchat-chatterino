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

/// Badges from Chatlee (https://chatlee.lol/developers): the badge each
/// person chose to wear. The catalog of badges is read every 30 minutes, who
/// wears which every minute, only what changed since the last time.
class ChatleeBadges final : public QObject
{
public:
    ChatleeBadges();

    std::optional<EmotePtr> getBadge(const QString &userID) const;

    /// Loads the catalog and all who wear a badge, replacing what was loaded
    /// before.
    ///
    /// @param onDone called once the answer is there, with whether loading
    ///               worked
    void load(const std::function<void(bool)> &onDone = {});

    /// The User-Agent Chatlee asks clients to send
    static QByteArray userAgent();
    /// The badge for a catalog entry, also for the badge picker
    static EmotePtr makeBadge(const QString &id, const QString &title,
                              const QString &imageUrl);

private:
    void loadCatalog(const std::function<void(bool)> &onDone);
    void loadWorn(bool full, const std::function<void(bool)> &onDone);
    bool applyCatalog(const QByteArray &payload);
    bool applyWorn(const QByteArray &payload);

    mutable std::shared_mutex mutex_;
    /// By catalog id and by slug
    std::unordered_map<QString, EmotePtr> catalog_;
    /// User id to the badge (id or slug) they wear
    std::unordered_map<QString, QString> worn_;
    QByteArray catalogEtag_;
    qint64 wornVersion_ = -1;
    bool loading_ = false;

    QTimer catalogTimer_;
    QTimer wornTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
