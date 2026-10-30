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
class Image;
using ImagePtr = std::shared_ptr<Image>;
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
    /// The frame (avatar decoration) `login` wears around their profile
    /// picture, or nullptr (looked up like paint()).
    ImagePtr frame(const QString &login);
    /// A paint from the catalog, or nullptr
    std::shared_ptr<Paint> paintById(const QString &id) const;
    /// A frame from the catalog, or nullptr
    ImagePtr frameById(const QString &id) const;
    /// Remembers what `login` wears and redraws when it changed. Without
    /// `frame` the frame stays as it was.
    void setWorn(const QString &login, const QString &paint,
                 const QString &badge,
                 const std::optional<QString> &frame = std::nullopt);
    /// Asks for what `login` wears ahead of drawing it, e.g. when their
    /// message arrives.
    void prefetch(const QString &login);

    std::optional<EmotePtr> channelEmote(const QString &roomId,
                                         EmoteNameView name) const;
    std::optional<EmotePtr> globalEmote(EmoteNameView name) const;
    /// A personal emote of `login`. Unknown chatters are looked up, so their
    /// personal emotes show from their next message on.
    std::optional<EmotePtr> personalEmote(const QString &login,
                                          EmoteNameView name);
    std::shared_ptr<const EmoteMap> channelEmotes(const QString &roomId) const;
    std::shared_ptr<const EmoteMap> globalEmotes() const;
    /// The name of a channel's active JilTV emote set, empty when unknown
    QString channelSetName(const QString &roomId) const;

    /// Loads a joined channel's JilTV emotes (nothing if it doesn't use
    /// JilTV). `done` gets the number of emotes, from the GUI thread.
    void loadChannelEmotes(const QString &roomId,
                           std::function<void(int)> done = {});
    void loadGlobalEmotes();

    /// The personal emotes of `login`, usable in every channel, or nullptr
    /// when they weren't loaded
    std::shared_ptr<const EmoteMap> personalEmotes(const QString &login) const;
    /// Loads the personal emotes of `login`. `done` runs in the GUI thread
    /// once they are there.
    void loadPersonalEmotes(const QString &login,
                            std::function<void()> done = {});
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

    struct Worn {
        QString paint;
        QString badge;
        QDateTime fetched;
        QString frame;
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
    std::unordered_map<QString, ImagePtr> frames_;
    std::unordered_map<QString, Worn> worn_;
    QSet<QString> pending_;
    QSet<QString> inFlight_;
    bool flushScheduled_ = false;
    std::unordered_map<QString, std::shared_ptr<const EmoteMap>> channels_;
    std::shared_ptr<const EmoteMap> global_;
    /// The name of each channel's active set, by room ID
    std::unordered_map<QString, QString> channelSetNames_;
    struct Personal {
        std::shared_ptr<const EmoteMap> emotes;
        QDateTime fetched;
    };
    /// By lowercase login. Guarded by mutex_.
    std::unordered_map<QString, Personal> personal_;
    QSet<QString> personalPending_;
    QSet<QString> personalInFlight_;
    bool personalFlushScheduled_ = false;
    void flushPersonalLookups();
    /// Stores the personal emotes of the logins in `users`, from a response
    /// of /v1/personal-emotes/twitch. Logins in `logins` that are missing in
    /// it have none.
    void storePersonalEmotes(const QStringList &logins,
                             const QJsonObject &root);

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
