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
///   (GET /v1/cosmetics/twitch?logins=&channel=), and kept for five
///   minutes. Answers are per channel: Supporters can wear an outfit in
///   some channels and show other personal emote presets there
/// - emotes: the global sets (GET /v1/sets/global) and each joined
///   channel's set (GET /v1/channels/twitch/{id}/bootstrap)
/// - live updates: an event stream per channel (emote changes, chatters
///   arriving with their cosmetics) and one for cosmetics and personal
///   emote changes
class Jiltv
{
public:
    static Jiltv &instance();

    /// The paint `login` wears in `channel` (its login; empty for no
    /// particular channel), or nullptr. Unknown chatters are looked up; the
    /// chat is redrawn when the answer arrives.
    std::shared_ptr<Paint> paint(const QString &login,
                                 const QString &channel = {});
    /// The badge `login` wears in `channel`, or nullptr (looked up like
    /// paint()).
    EmotePtr badge(const QString &login, const QString &channel = {});
    /// The frame (avatar decoration) `login` wears around their profile
    /// picture in `channel`, or nullptr (looked up like paint()).
    ImagePtr frame(const QString &login, const QString &channel = {});
    /// URL of the profile banner of `login` for usercards (5:2, 600×240,
    /// animated WebP), or empty (looked up like paint()). Only Supporters
    /// have one, and only once a moderator approved it. A URL rather than an
    /// Image: banners have more frames than Image keeps in memory.
    QString bannerUrl(const QString &login);
    /// A paint from the catalog, or nullptr
    std::shared_ptr<Paint> paintById(const QString &id) const;
    /// A frame from the catalog, or nullptr
    ImagePtr frameById(const QString &id) const;
    /// Remembers what `login` wears and redraws when it changed. Without
    /// `frame` (or `banner`) it stays as it was.
    void setWorn(const QString &login, const QString &paint,
                 const QString &badge,
                 const std::optional<QString> &frame = std::nullopt,
                 const std::optional<QString> &banner = std::nullopt);
    /// Asks for what `login` wears in `channel` ahead of drawing it, e.g.
    /// when their message arrives.
    void prefetch(const QString &login, const QString &channel = {});

    std::optional<EmotePtr> channelEmote(const QString &roomId,
                                         EmoteNameView name) const;
    std::optional<EmotePtr> globalEmote(EmoteNameView name) const;
    /// A personal emote of `login` in `channel` (a preset can be assigned to
    /// it). Unknown chatters are looked up, so their personal emotes show
    /// from their next message on.
    std::optional<EmotePtr> personalEmote(const QString &login,
                                          EmoteNameView name,
                                          const QString &channel = {});
    std::shared_ptr<const EmoteMap> channelEmotes(const QString &roomId) const;
    std::shared_ptr<const EmoteMap> globalEmotes() const;
    /// The name of a channel's active JilTV emote set, empty when unknown
    QString channelSetName(const QString &roomId) const;

    /// Loads a joined channel's JilTV emotes (nothing if it doesn't use
    /// JilTV). `done` gets the number of emotes, from the GUI thread.
    /// A closed chat: drops its event stream and emotes (GUI thread).
    void unfollow(const QString &roomId);
    void loadChannelEmotes(const QString &roomId,
                           std::function<void(int)> done = {});
    void loadGlobalEmotes();

    /// The personal emotes of `login` in `channel`, or nullptr when they
    /// weren't loaded
    std::shared_ptr<const EmoteMap> personalEmotes(
        const QString &login, const QString &channel = {}) const;
    /// Loads the personal emotes of `login` in `channel`. `done` runs in the
    /// GUI thread once they are there.
    void loadPersonalEmotes(const QString &login,
                            std::function<void()> done = {},
                            const QString &channel = {});
    void loadCatalog();

    /// Tells JilTV which JilTV channels the signed-in user is in, so other
    /// clients show their personal emotes and cosmetics there from their
    /// first message. Runs every few minutes; call it after signing in.
    void updatePresence();

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
        /// Profile banner ID: {cdn}/banners/{id}/card.webp
        QString banner;
    };

    /// Returns the cached answer for `login` in `channel`, and queues a
    /// lookup when there is none or it is old.
    std::optional<Worn> worn(const QString &login, const QString &channel);
    void flushLookups();
    /// Stores what `login` wears under the cache key `key`; true when it
    /// changed. Guarded by mutex_ (held by the caller).
    bool storeWorn(const QString &key, const QString &paint,
                   const QString &badge, const std::optional<QString> &frame,
                   const std::optional<QString> &banner, bool &badgeChanged);
    /// The login has outfits or presets in some channels: every channel it
    /// is cached for is asked again, past the API's cache. GUI thread.
    void refreshWorn(const QString &login);
    void refreshPersonal(const QString &login);
    void applyCatalog(const QJsonObject &root);
    /// Points paints with day phases at the look for now; repaints the chat
    /// when one changed.
    void updatePhases();

    mutable std::mutex mutex_;
    /// Guarded by mutex_.
    std::unordered_map<QString, std::shared_ptr<Paint>> paints_;
    std::unordered_map<QString, EmotePtr> badges_;
    std::unordered_map<QString, ImagePtr> frames_;
    /// Where catalog files live (from the catalog)
    QString cdn_;
    /// By "login#channel" (channel empty for none).
    std::unordered_map<QString, Worn> worn_;
    QSet<QString> pending_;
    QSet<QString> inFlight_;
    /// Pending keys to ask for past the API's cache (after a change).
    QSet<QString> fresh_;
    bool flushScheduled_ = false;
    std::unordered_map<QString, std::shared_ptr<const EmoteMap>> channels_;
    std::shared_ptr<const EmoteMap> global_;
    /// The name of each channel's active set, by room ID
    std::unordered_map<QString, QString> channelSetNames_;
    struct Personal {
        std::shared_ptr<const EmoteMap> emotes;
        QDateTime fetched;
    };
    /// By "login#channel" like worn_. Guarded by mutex_.
    std::unordered_map<QString, Personal> personal_;
    QSet<QString> personalPending_;
    QSet<QString> personalInFlight_;
    QSet<QString> personalFresh_;
    bool personalFlushScheduled_ = false;
    void flushPersonalLookups();
    /// Stores personal emotes under the cache keys `keys` ("login#channel"),
    /// from a response of /v1/personal-emotes/twitch (users by login). Keys
    /// whose login is missing in it have none.
    void storePersonalEmotes(const QStringList &keys, const QJsonObject &root);

    QTimer catalogTimer_;
    QTimer presenceTimer_;
    /// Switches paints with looks by time of day (updatePhases).
    QTimer phaseTimer_;
    /// Reports being in one channel, when signed in with JilTV.
    void reportPresence(const QString &roomId);
    /// The JilTV channel ID of each channel using JilTV, by room ID.
    /// Guarded by mutex_.
    std::unordered_map<QString, QString> channelIds_;

    /// Live updates; GUI thread only. Torn down when the app quits, before
    /// the static instance itself goes away.
    QNetworkAccessManager *network_ = nullptr;
    std::unordered_map<QString, std::unique_ptr<JiltvStream>> channelStreams_;
    std::unique_ptr<JiltvStream> cosmeticsStream_;
};

/// Whether JilTV emotes come first among the emote providers (setting).
bool jiltvFirst();

}  // namespace chatterino
