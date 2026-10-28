// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;
class Paint;
class JiltvStream;
}  // namespace chatterino

class QNetworkAccessManager;

namespace chatterino {

/// JilTV (jiltv.app): paints and badges of chatters and the emotes of
/// channels, read from the same public API the browser extension uses.
///
/// - the cosmetics catalog (GET /v1/cosmetics/catalog) has every paint and
///   badge; it is loaded at start and every ten minutes
/// - what a chatter wears is looked up by login in batches of up to 100
///   (GET /v1/cosmetics/twitch?logins=), and kept for five minutes
/// - emotes: the global sets (GET /v1/sets/global) and each joined
///   channel's set (GET /v1/channels/twitch/{id}/bootstrap)
/// - live updates: an event stream per channel (emote changes, chatters
///   arriving with their cosmetics) and one for cosmetics changes
class Jiltv
{
public:
    static Jiltv &instance();

    /// The paint `login` wears, or nullptr. Unknown chatters are looked up;
    /// the chat is redrawn when the answer arrives.
    std::shared_ptr<Paint> paint(const QString &login);
    /// The badge `login` wears, or nullptr (looked up like paint()).
    EmotePtr badge(const QString &login);
    /// Asks for what `login` wears ahead of drawing it, e.g. when their
    /// message arrives.
    void prefetch(const QString &login);

    std::optional<EmotePtr> channelEmote(const QString &roomId,
                                         EmoteNameView name) const;
    std::optional<EmotePtr> globalEmote(EmoteNameView name) const;
    std::shared_ptr<const EmoteMap> channelEmotes(const QString &roomId) const;
    std::shared_ptr<const EmoteMap> globalEmotes() const;

    /// Loads a joined channel's JilTV emotes (nothing if it doesn't use
    /// JilTV). `done` gets the number of emotes, from the GUI thread.
    void loadChannelEmotes(const QString &roomId,
                           std::function<void(int)> done = {});
    void loadGlobalEmotes();
    void loadCatalog();

    QString apiUrl() const;

private:
    Jiltv();
    ~Jiltv();

    /// Follows a channel's events (once per channel and API).
    void follow(const QString &roomId);
    void followCosmetics();
    void onChannelEvent(const QString &roomId, const QString &type,
                        const QJsonObject &data);
    /// Remembers what `login` wears and redraws when it changed.
    void setWorn(const QString &login, const QString &paint,
                 const QString &badge);

    struct Worn {
        QString paint;
        QString badge;
        QDateTime fetched;
    };

    /// Returns the cached answer for `login`, and queues a lookup when there
    /// is none or it is old.
    std::optional<Worn> worn(const QString &login);
    void flushLookups();
    void applyCatalog(const QJsonObject &root);

    mutable std::mutex mutex_;
    /// Guarded by mutex_.
    std::unordered_map<QString, std::shared_ptr<Paint>> paints_;
    std::unordered_map<QString, EmotePtr> badges_;
    std::unordered_map<QString, Worn> worn_;
    QSet<QString> pending_;
    QSet<QString> inFlight_;
    bool flushScheduled_ = false;
    std::unordered_map<QString, std::shared_ptr<const EmoteMap>> channels_;
    std::shared_ptr<const EmoteMap> global_;

    QTimer catalogTimer_;

    /// Live updates; GUI thread only. Torn down when the app quits, before
    /// the static instance itself goes away.
    QNetworkAccessManager *network_ = nullptr;
    std::unordered_map<QString, std::unique_ptr<JiltvStream>> channelStreams_;
    std::unique_ptr<JiltvStream> cosmeticsStream_;
};

/// Whether JilTV emotes come first among the emote providers (setting).
bool jiltvFirst();

}  // namespace chatterino
