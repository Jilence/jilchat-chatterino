// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jiltv/Jiltv.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/jiltv/JiltvAuth.hpp"
#include "providers/jiltv/JiltvPaint.hpp"
#include "providers/jiltv/JiltvStream.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QUrlQuery>

#include <cstdint>
#include <tuple>
#include <vector>

namespace chatterino {

using namespace Qt::Literals;

namespace {

constexpr int LOOKUP_BATCH = 100;
constexpr int LOOKUP_DELAY_MS = 150;
constexpr qint64 WORN_TTL_SECS = 5 * 60;
constexpr int CATALOG_INTERVAL_MS = 10 * 60 * 1000;
/// A presence expires after 10 minutes
constexpr int PRESENCE_INTERVAL_MS = 5 * 60 * 1000;
constexpr auto PRESENCE_MUTATION =
    "mutation($c: ID!) { updatePresence(channelId: $c) { lastSeenAt } }";

constexpr int FLAG_ZERO_WIDTH = 2;
constexpr int FLAG_PENDING = 16;

const QString WEB_URL = QStringLiteral("https://jiltv.app");

QString rarityName(const QString &r)
{
    static const std::unordered_map<QString, QString> names{
        {QStringLiteral("COMMON"), QStringLiteral("Common")},
        {QStringLiteral("UNCOMMON"), QStringLiteral("Uncommon")},
        {QStringLiteral("RARE"), QStringLiteral("Rare")},
        {QStringLiteral("EPIC"), QStringLiteral("Epic")},
        {QStringLiteral("LEGENDARY"), QStringLiteral("Legendary")},
        {QStringLiteral("MYTHIC"), QStringLiteral("Mythic")},
    };
    const auto it = names.find(r);
    return it == names.end() ? QString{} : it->second;
}

/// One set's emotes; `kind` goes into the tooltip ("Channel", "Global").
EmoteMap parseEmotes(const QJsonArray &emotes, const QString &cdn,
                     const QString &kind)
{
    EmoteMap map;
    const bool showPending = getSettings()->jiltvShowPendingEmotes;
    for (const auto &value : emotes)
    {
        const auto e = value.toObject();
        const auto id = e.value("id").toString();
        const auto name = e.value("name").toString();
        const int flags = e.value("f").toInt();
        if (id.isEmpty() || name.isEmpty() ||
            ((flags & FLAG_PENDING) != 0 && !showPending))
        {
            continue;
        }
        const auto version = QString::number(e.value("v").toInt());
        const QSize base(std::max(1, e.value("w").toInt(28)),
                         std::max(1, e.value("h").toInt(28)));
        const auto url = [&](int scale) {
            return Url{u"%1/%2/%3/%4x.webp"_s.arg(cdn, id, version,
                                                  QString::number(scale))};
        };
        auto tooltip = name.toHtmlEscaped() + QStringLiteral("<br>JilTV ") +
                       kind + QStringLiteral(" Emote");
        const auto original = e.value("original_name").toString();
        if (!original.isEmpty() && original != name)
        {
            tooltip +=
                QStringLiteral("<br>Alias of ") + original.toHtmlEscaped();
        }
        Emote emote{
            .name = EmoteName{name},
            .images =
                ImageSet{
                    Image::fromUrl(url(1), 1, base),
                    Image::fromUrl(url(2), 0.5, base * 2),
                    Image::fromUrl(url(4), 0.25, base * 4),
                },
            .tooltip = Tooltip{tooltip},
            .homePage = Url{WEB_URL + QStringLiteral("/emotes/") + id},
            .zeroWidth =
                e.value("zwo").toBool() || (flags & FLAG_ZERO_WIDTH) != 0,
            .id = EmoteId{id},
        };
        if (!original.isEmpty() && original != name)
        {
            emote.baseName = EmoteName{original};
        }
        // The key first: the right side of = runs first and moves `emote`.
        auto key = emote.name;
        map[key] = std::make_shared<const Emote>(std::move(emote));
    }
    return map;
}

/// The emotes of a bootstrap answer: `{set: {emotes}}` or `{sets: [...]}`.
EmoteMap parseBootstrap(const QJsonObject &root, const QString &kind)
{
    const auto cdn = root.value("cdn").toString();
    if (root.contains("sets"))
    {
        EmoteMap map;
        for (const auto &set : root.value("sets").toArray())
        {
            for (auto &&[k, v] : parseEmotes(
                     set.toObject().value("emotes").toArray(), cdn, kind))
            {
                // The first set that has a name wins, like in the extension.
                map.try_emplace(k, v);
            }
        }
        return map;
    }
    return parseEmotes(root.value("set").toObject().value("emotes").toArray(),
                       cdn, kind);
}

}  // namespace

Jiltv &Jiltv::instance()
{
    static Jiltv instance;
    return instance;
}

Jiltv::Jiltv()
    : global_(std::make_shared<const EmoteMap>())
{
    this->catalogTimer_.setInterval(CATALOG_INTERVAL_MS);
    QObject::connect(&this->catalogTimer_, &QTimer::timeout, [this] {
        this->loadCatalog();
    });
    this->catalogTimer_.start();
    this->presenceTimer_.setInterval(PRESENCE_INTERVAL_MS);
    QObject::connect(&this->presenceTimer_, &QTimer::timeout,
                     &this->presenceTimer_, [this] {
                         this->updatePresence();
                     });
    this->presenceTimer_.start();
    this->network_ = new QNetworkAccessManager();
    QObject::connect(QCoreApplication::instance(),
                     &QCoreApplication::aboutToQuit, [this] {
                         this->channelStreams_.clear();
                         this->cosmeticsStream_.reset();
                         delete this->network_;
                         this->network_ = nullptr;
                     });
}

Jiltv::~Jiltv() = default;

QString Jiltv::apiUrl() const
{
    auto url = getSettings()->jiltvApiUrl.getValue().trimmed();
    while (url.endsWith(u'/'))
    {
        url.chop(1);
    }
    return url.isEmpty() ? QStringLiteral("https://api.jiltv.app") : url;
}

// --- cosmetics ---

void Jiltv::loadCatalog()
{
    if (!getSettings()->enableJiltv)
    {
        return;
    }
    NetworkRequest(
        QUrl(this->apiUrl() + QStringLiteral("/v1/cosmetics/catalog")))
        .timeout(30000)
        .onSuccess([this](const NetworkResult &result) {
            this->applyCatalog(result.parseJson());
            this->followCosmetics();
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoSeventv)
                << "JilTV catalog failed:" << result.formatError();
        })
        .execute();
}

void Jiltv::applyCatalog(const QJsonObject &root)
{
    const auto cdn = root.value("cdn").toString();
    std::unordered_map<QString, std::shared_ptr<Paint>> paints;
    for (const auto &value : root.value("paints").toArray())
    {
        const auto p = value.toObject();
        const auto id = p.value("id").toString();
        if (id.isEmpty())
        {
            continue;
        }
        auto name = p.value("name").toString();
        if (const auto r = rarityName(p.value("r").toString()); !r.isEmpty())
        {
            name += QStringLiteral(" (") + r + QStringLiteral(")");
        }
        paints[id] =
            std::make_shared<JiltvPaint>(id, name, p.value("data").toObject(),
                                         cdn, p.value("texture").toObject());
    }

    std::unordered_map<QString, EmotePtr> badges;
    for (const auto &value : root.value("badges").toArray())
    {
        const auto b = value.toObject();
        const auto id = b.value("id").toString();
        const auto data = b.value("data").toObject();
        const auto image = data.value("image").toString();
        if (id.isEmpty() || image.isEmpty())
        {
            continue;
        }
        const Url url{cdn + QStringLiteral("/badges/") + image};
        const QSize base(18, 18);
        auto tooltip = b.value("name").toString().toHtmlEscaped();
        if (const auto r = rarityName(b.value("r").toString()); !r.isEmpty())
        {
            tooltip += QStringLiteral(" (") + r + QStringLiteral(")");
        }
        tooltip += QStringLiteral("<br>JilTV Badge");
        badges[id] = std::make_shared<const Emote>(Emote{
            .name = EmoteName{QStringLiteral("jiltv:") + id},
            .images =
                ImageSet{
                    Image::fromAutoscaledUrl(url, 18),
                    Image::fromUrl(url, 0.5, base * 2),
                    Image::fromUrl(url, 0.25, base * 4),
                },
            .tooltip = Tooltip{tooltip},
            .homePage = Url{WEB_URL},
            .id = EmoteId{id},
        });
    }

    // Frames are square, transparent in the middle, and drawn at 120 % of
    // the profile picture
    std::unordered_map<QString, ImagePtr> frames;
    const auto decorations = root.value("decorations").toArray();
    for (const auto &value : decorations)
    {
        const auto d = value.toObject();
        const auto id = d.value("id").toString();
        const auto image = d.value("data").toObject().value("image").toString();
        if (id.isEmpty() || image.isEmpty())
        {
            continue;
        }
        frames[id] =
            Image::fromUrl(Url{cdn + QStringLiteral("/decorations/") + image});
    }

    {
        const std::scoped_lock lock(this->mutex_);
        this->paints_ = std::move(paints);
        this->badges_ = std::move(badges);
        this->frames_ = std::move(frames);
        this->cdn_ = cdn;
    }
    getApp()->getWindows()->invalidateChannelViewBuffers();
    WindowManager::notifyBadgesUpdated();
}

namespace {

/// Cache key of a login in a channel ("login#channel"; channel empty for
/// none): answers differ per channel (outfits, personal presets).
QString cacheKey(const QString &login, const QString &channel)
{
    return login.toLower() + QChar(u'#') + channel.toLower();
}

QString keyLogin(const QString &key)
{
    return key.section(u'#', 0, 0);
}

QString keyChannel(const QString &key)
{
    return key.section(u'#', 1);
}

/// Takes up to `limit` keys of one channel out of `pending` (one request
/// answers one channel). Returns the channel and whether a key in the batch
/// was in `fresh` (asked past the API's cache).
std::pair<QString, bool> takeBatch(QSet<QString> &pending,
                                   QSet<QString> &inFlight,
                                   QSet<QString> &fresh, QStringList &batch,
                                   qsizetype limit)
{
    if (pending.isEmpty())
    {
        return {};
    }
    const auto channel = keyChannel(*pending.begin());
    bool anyFresh = false;
    for (auto it = pending.begin();
         it != pending.end() && batch.size() < limit;)
    {
        if (keyChannel(*it) != channel)
        {
            ++it;
            continue;
        }
        batch.append(*it);
        inFlight.insert(*it);
        anyFresh |= fresh.remove(*it);
        it = pending.erase(it);
    }
    return {channel, anyFresh};
}

/// The lookup URL for the logins of `keys` in `channel`.
QUrl lookupUrl(const QString &api, const QString &path, const QStringList &keys,
               const QString &channel, bool fresh)
{
    QStringList logins;
    for (const auto &k : keys)
    {
        logins.append(keyLogin(k));
    }
    QUrl url(api + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("logins"), logins.join(u','));
    if (!channel.isEmpty())
    {
        query.addQueryItem(QStringLiteral("channel"), channel);
    }
    if (fresh)
    {
        // Right after a change: past the minute the API's answers are cached.
        query.addQueryItem(
            QStringLiteral("t"),
            QString::number(QDateTime::currentMSecsSinceEpoch()));
    }
    url.setQuery(query);
    return url;
}

}  // namespace

std::optional<Jiltv::Worn> Jiltv::worn(const QString &login,
                                       const QString &channel)
{
    if (!getSettings()->enableJiltv || login.isEmpty())
    {
        return std::nullopt;
    }
    const auto key = cacheKey(login, channel);
    const std::scoped_lock lock(this->mutex_);
    std::optional<Worn> out;
    const auto it = this->worn_.find(key);
    if (it != this->worn_.end())
    {
        out = it->second;
        if (it->second.fetched.secsTo(QDateTime::currentDateTimeUtc()) <
            WORN_TTL_SECS)
        {
            return out;
        }
    }
    if (!this->inFlight_.contains(key))
    {
        this->pending_.insert(key);
        if (!this->flushScheduled_)
        {
            this->flushScheduled_ = true;
            postToThread([this] {
                QTimer::singleShot(LOOKUP_DELAY_MS, [this] {
                    this->flushLookups();
                });
            });
        }
    }
    return out;
}

bool Jiltv::storeWorn(const QString &key, const QString &paint,
                      const QString &badge, const std::optional<QString> &frame,
                      const std::optional<QString> &banner, bool &badgeChanged)
{
    auto &slot = this->worn_[key];
    const auto newFrame = frame.value_or(slot.frame);
    const auto newBanner = banner.value_or(slot.banner);
    const bool changed = slot.paint != paint || slot.badge != badge ||
                         slot.frame != newFrame || slot.banner != newBanner;
    badgeChanged |= slot.badge != badge;
    slot = {
        .paint = paint,
        .badge = badge,
        .fetched = QDateTime::currentDateTimeUtc(),
        .frame = newFrame,
        .banner = newBanner,
    };
    return changed;
}

void Jiltv::flushLookups()
{
    QStringList batch;
    QString channel;
    bool fresh = false;
    {
        const std::scoped_lock lock(this->mutex_);
        this->flushScheduled_ = false;
        std::tie(channel, fresh) = takeBatch(this->pending_, this->inFlight_,
                                             this->fresh_, batch, LOOKUP_BATCH);
        if (!this->pending_.isEmpty())
        {
            this->flushScheduled_ = true;
            QTimer::singleShot(LOOKUP_DELAY_MS, [this] {
                this->flushLookups();
            });
        }
    }
    if (batch.isEmpty())
    {
        return;
    }

    // `fresh`: asked again after a change, so names already drawn get their
    // new badge too (badges are part of the built message).
    const auto finish = [this, batch, fresh](const QJsonObject &users,
                                             bool ok) {
        bool changed = false;
        bool badgeChanged = false;
        {
            const std::scoped_lock lock(this->mutex_);
            for (const auto &key : batch)
            {
                this->inFlight_.remove(key);
                if (!ok)
                {
                    continue;  // asked again the next time it's drawn
                }
                const auto u = users.value(keyLogin(key)).toObject();
                changed |= this->storeWorn(
                    key, u.value("p").toString(), u.value("b").toString(),
                    u.value("d").toString(), u.value("bn").toString(),
                    badgeChanged);
            }
        }
        if (changed)
        {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        }
        if (fresh && badgeChanged)
        {
            WindowManager::notifyBadgesUpdated();
        }
    };

    NetworkRequest(lookupUrl(this->apiUrl(),
                             QStringLiteral("/v1/cosmetics/twitch"), batch,
                             channel, fresh))
        .timeout(20000)
        .onSuccess([finish](const NetworkResult &result) {
            finish(result.parseJson().value("users").toObject(), true);
        })
        .onError([finish](const NetworkResult &) {
            finish({}, false);
        })
        .execute();
}

void Jiltv::refreshWorn(const QString &login)
{
    const QString prefix = login.toLower() + QChar(u'#');
    {
        const std::scoped_lock lock(this->mutex_);
        bool any = false;
        for (const auto &[key, w] : this->worn_)
        {
            if (key.startsWith(prefix) && !this->inFlight_.contains(key))
            {
                this->pending_.insert(key);
                this->fresh_.insert(key);
                any = true;
            }
        }
        if (!any || this->flushScheduled_)
        {
            return;
        }
        this->flushScheduled_ = true;
    }
    QTimer::singleShot(0, [this] {
        this->flushLookups();
    });
}

void Jiltv::prefetch(const QString &login, const QString &channel)
{
    (void)this->worn(login, channel);
}

std::shared_ptr<Paint> Jiltv::paint(const QString &login,
                                    const QString &channel)
{
    const auto w = this->worn(login, channel);
    if (!w || w->paint.isEmpty())
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->paints_.find(w->paint);
    return it == this->paints_.end() ? nullptr : it->second;
}

ImagePtr Jiltv::frame(const QString &login, const QString &channel)
{
    if (!getSettings()->showJiltvFrames)
    {
        return nullptr;
    }
    const auto w = this->worn(login, channel);
    if (!w || w->frame.isEmpty())
    {
        return nullptr;
    }
    return this->frameById(w->frame);
}

QString Jiltv::bannerUrl(const QString &login)
{
    if (!getSettings()->showJiltvBanners)
    {
        return {};
    }
    // The banner is the same in every channel.
    const auto w = this->worn(login, {});
    if (!w || w->banner.isEmpty())
    {
        return {};
    }
    const std::scoped_lock lock(this->mutex_);
    if (this->cdn_.isEmpty())
    {
        return {};  // the catalog isn't there yet
    }
    return this->cdn_ + QStringLiteral("/banners/") + w->banner +
           QStringLiteral("/card.webp");
}

ImagePtr Jiltv::frameById(const QString &id) const
{
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->frames_.find(id);
    return it == this->frames_.end() ? nullptr : it->second;
}

std::shared_ptr<Paint> Jiltv::paintById(const QString &id) const
{
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->paints_.find(id);
    return it == this->paints_.end() ? nullptr : it->second;
}

EmotePtr Jiltv::badge(const QString &login, const QString &channel)
{
    if (!getSettings()->showBadgesJiltv)
    {
        return nullptr;
    }
    const auto w = this->worn(login, channel);
    if (!w || w->badge.isEmpty())
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->badges_.find(w->badge);
    return it == this->badges_.end() ? nullptr : it->second;
}

// --- emotes ---

void Jiltv::loadGlobalEmotes()
{
    if (!getSettings()->enableJiltv || !getSettings()->enableJiltvEmotes)
    {
        const std::scoped_lock lock(this->mutex_);
        this->global_ = std::make_shared<const EmoteMap>();
        return;
    }
    NetworkRequest(QUrl(this->apiUrl() + QStringLiteral("/v1/sets/global")))
        .timeout(30000)
        .onSuccess([this](const NetworkResult &result) {
            auto map = std::make_shared<const EmoteMap>(
                parseBootstrap(result.parseJson(), QStringLiteral("Global")));
            const std::scoped_lock lock(this->mutex_);
            this->global_ = std::move(map);
        })
        .execute();
}

void Jiltv::loadChannelEmotes(const QString &roomId,
                              std::function<void(int)> done)
{
    if (roomId.isEmpty() || !getSettings()->enableJiltv ||
        !getSettings()->enableJiltvEmotes)
    {
        {
            const std::scoped_lock lock(this->mutex_);
            this->channels_.erase(roomId);
            this->channelSetNames_.erase(roomId);
        }
        if (done)
        {
            done(0);
        }
        return;
    }
    NetworkRequest(QUrl(this->apiUrl() +
                        QStringLiteral("/v1/channels/twitch/") + roomId +
                        QStringLiteral("/bootstrap")))
        .timeout(30000)
        .onSuccess([this, roomId, done](const NetworkResult &result) {
            const auto root = result.parseJson();
            auto map = std::make_shared<const EmoteMap>(
                parseBootstrap(root, QStringLiteral("Channel")));
            auto setName =
                root.value("set").toObject().value("name").toString();
            const auto channelId =
                root.value("channel").toObject().value("id").toString();
            const auto n = static_cast<int>(map->size());
            qCDebug(chatterinoSeventv)
                << "JilTV: channel" << roomId << "has" << n << "emotes";
            {
                const std::scoped_lock lock(this->mutex_);
                this->channels_[roomId] = std::move(map);
                this->channelSetNames_[roomId] = std::move(setName);
                this->channelIds_[roomId] = channelId;
            }
            this->follow(roomId);
            this->reportPresence(roomId);
            if (done)
            {
                done(n);
            }
        })
        .onError([this, roomId, done](const NetworkResult &result) {
            // 404: the channel doesn't use JilTV.
            qCWarning(chatterinoSeventv)
                << "JilTV: channel" << roomId
                << "emotes failed:" << result.formatError();
            {
                const std::scoped_lock lock(this->mutex_);
                this->channels_.erase(roomId);
                this->channelSetNames_.erase(roomId);
            }
            if (done)
            {
                done(0);
            }
        })
        .execute();
}

void Jiltv::updatePresence()
{
    getApp()->getTwitch()->forEachChannel([this](const ChannelPtr &chan) {
        if (const auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            this->reportPresence(channel->roomId());
        }
    });
}

void Jiltv::reportPresence(const QString &roomId)
{
    if (!getSettings()->enableJiltv || roomId.isEmpty())
    {
        return;
    }
    QString channelId;
    {
        const std::scoped_lock lock(this->mutex_);
        const auto it = this->channelIds_.find(roomId);
        if (it == this->channelIds_.end() || it->second.isEmpty())
        {
            return;  // the channel doesn't use JilTV
        }
        channelId = it->second;
    }

    JiltvAuth::instance().load(QCoreApplication::instance(), [channelId] {
        auto &auth = JiltvAuth::instance();
        if (!auth.signedIn())
        {
            return;
        }
        auth.graphql(
            QString::fromLatin1(PRESENCE_MUTATION), {{"c", channelId}},
            [](const QJsonObject &) {},
            [channelId](const QString &error) {
                qCDebug(chatterinoSeventv)
                    << "JilTV: presence in" << channelId << "failed:" << error;
            });
    });
}

std::shared_ptr<const EmoteMap> Jiltv::channelEmotes(
    const QString &roomId) const
{
    if (!getSettings()->enableJiltv || !getSettings()->enableJiltvEmotes)
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->channels_.find(roomId);
    return it == this->channels_.end() ? nullptr : it->second;
}

void Jiltv::loadPersonalEmotes(const QString &login, std::function<void()> done,
                               const QString &channel)
{
    if (login.isEmpty() || !getSettings()->enableJiltv ||
        !getSettings()->enableJiltvEmotes)
    {
        return;
    }
    const auto key = cacheKey(login, channel);
    NetworkRequest(lookupUrl(this->apiUrl(),
                             QStringLiteral("/v1/personal-emotes/twitch"),
                             {key}, keyChannel(key), false))
        .timeout(30000)
        .onSuccess(
            [this, key, done = std::move(done)](const NetworkResult &result) {
                this->storePersonalEmotes({key}, result.parseJson());
                if (done)
                {
                    done();
                }
            })
        .execute();
}

void Jiltv::storePersonalEmotes(const QStringList &keys,
                                const QJsonObject &root)
{
    const auto users = root.value("users").toObject();
    const auto cdn = root.value("cdn").toString();
    const auto now = QDateTime::currentDateTimeUtc();
    const std::scoped_lock lock(this->mutex_);
    for (const auto &key : keys)
    {
        this->personal_[key] = {
            .emotes = std::make_shared<const EmoteMap>(
                parseEmotes(users.value(keyLogin(key)).toArray(), cdn,
                            QStringLiteral("Personal"))),
            .fetched = now,
        };
    }
}

std::optional<EmotePtr> Jiltv::personalEmote(const QString &login,
                                             EmoteNameView name,
                                             const QString &channel)
{
    if (login.isEmpty() || !getSettings()->enableJiltv ||
        !getSettings()->enableJiltvEmotes)
    {
        return std::nullopt;
    }
    const auto key = cacheKey(login, channel);
    const std::scoped_lock lock(this->mutex_);
    std::shared_ptr<const EmoteMap> emotes;
    bool fresh = false;
    if (const auto it = this->personal_.find(key); it != this->personal_.end())
    {
        emotes = it->second.emotes;
        fresh = it->second.fetched.secsTo(QDateTime::currentDateTimeUtc()) <
                WORN_TTL_SECS;
    }
    if (!fresh && !this->personalInFlight_.contains(key))
    {
        this->personalPending_.insert(key);
        if (!this->personalFlushScheduled_)
        {
            this->personalFlushScheduled_ = true;
            postToThread([this] {
                QTimer::singleShot(LOOKUP_DELAY_MS, [this] {
                    this->flushPersonalLookups();
                });
            });
        }
    }
    if (!emotes)
    {
        return std::nullopt;
    }
    const auto it = emotes->find(name);
    return it == emotes->end() ? std::nullopt : std::optional(it->second);
}

void Jiltv::flushPersonalLookups()
{
    QStringList batch;
    QString channel;
    bool fresh = false;
    {
        const std::scoped_lock lock(this->mutex_);
        this->personalFlushScheduled_ = false;
        std::tie(channel, fresh) =
            takeBatch(this->personalPending_, this->personalInFlight_,
                      this->personalFresh_, batch, LOOKUP_BATCH);
        if (!this->personalPending_.isEmpty())
        {
            this->personalFlushScheduled_ = true;
            QTimer::singleShot(LOOKUP_DELAY_MS, [this] {
                this->flushPersonalLookups();
            });
        }
    }
    if (batch.isEmpty())
    {
        return;
    }

    const auto done = [this, batch] {
        const std::scoped_lock lock(this->mutex_);
        for (const auto &key : batch)
        {
            this->personalInFlight_.remove(key);
        }
    };
    NetworkRequest(lookupUrl(this->apiUrl(),
                             QStringLiteral("/v1/personal-emotes/twitch"),
                             batch, channel, fresh))
        .timeout(20000)
        .onSuccess([this, batch, done](const NetworkResult &result) {
            this->storePersonalEmotes(batch, result.parseJson());
            done();
        })
        .onError([done](const NetworkResult &) {
            done();  // asked again with the next message
        })
        .execute();
}

void Jiltv::refreshPersonal(const QString &login)
{
    const QString prefix = login.toLower() + QChar(u'#');
    {
        const std::scoped_lock lock(this->mutex_);
        bool any = false;
        for (const auto &[key, p] : this->personal_)
        {
            if (key.startsWith(prefix) &&
                !this->personalInFlight_.contains(key))
            {
                this->personalPending_.insert(key);
                this->personalFresh_.insert(key);
                any = true;
            }
        }
        if (!any || this->personalFlushScheduled_)
        {
            return;
        }
        this->personalFlushScheduled_ = true;
    }
    QTimer::singleShot(0, [this] {
        this->flushPersonalLookups();
    });
}

std::shared_ptr<const EmoteMap> Jiltv::personalEmotes(
    const QString &login, const QString &channel) const
{
    if (!getSettings()->enableJiltv || !getSettings()->enableJiltvEmotes)
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->personal_.find(cacheKey(login, channel));
    return it == this->personal_.end() ? nullptr : it->second.emotes;
}

QString Jiltv::channelSetName(const QString &roomId) const
{
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->channelSetNames_.find(roomId);
    return it == this->channelSetNames_.end() ? QString{} : it->second;
}

std::shared_ptr<const EmoteMap> Jiltv::globalEmotes() const
{
    if (!getSettings()->enableJiltv || !getSettings()->enableJiltvEmotes)
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    return this->global_;
}

std::optional<EmotePtr> Jiltv::channelEmote(const QString &roomId,
                                            EmoteNameView name) const
{
    const auto map = this->channelEmotes(roomId);
    if (!map)
    {
        return std::nullopt;
    }
    const auto it = map->find(name);
    return it == map->end() ? std::nullopt : std::optional(it->second);
}

std::optional<EmotePtr> Jiltv::globalEmote(EmoteNameView name) const
{
    const auto map = this->globalEmotes();
    if (!map)
    {
        return std::nullopt;
    }
    const auto it = map->find(name);
    return it == map->end() ? std::nullopt : std::optional(it->second);
}

// --- live updates ---

void Jiltv::follow(const QString &roomId)
{
    if (this->network_ == nullptr)
    {
        return;
    }
    const QUrl url(this->apiUrl() +
                   QStringLiteral("/v1/events/channels/twitch/") + roomId);
    auto &stream = this->channelStreams_[roomId];
    if (stream && stream->url() == url)
    {
        return;
    }
    stream = std::make_unique<JiltvStream>(
        this->network_, url,
        [this, roomId](const QString &type, const QJsonObject &data) {
            this->onChannelEvent(roomId, type, data);
        });
}

void Jiltv::followCosmetics()
{
    if (this->network_ == nullptr)
    {
        return;
    }
    const QUrl url(this->apiUrl() + QStringLiteral("/v1/events/cosmetics"));
    if (this->cosmeticsStream_ && this->cosmeticsStream_->url() == url)
    {
        return;
    }
    this->cosmeticsStream_ = std::make_unique<JiltvStream>(
        this->network_, url,
        [this](const QString &type, const QJsonObject &data) {
            const auto login = data.value("login").toString().toLower();
            if (login.isEmpty())
            {
                return;
            }
            if (type == u"user.cosmetics")
            {
                // "ch": they wear outfits in some channels (not named, so
                // nobody can list them). The fields are the global look;
                // every channel they are shown in here is asked again.
                if (data.value("ch").toBool())
                {
                    this->refreshWorn(login);
                    return;
                }
                this->setWorn(login, data.value("p").toString(),
                              data.value("b").toString(),
                              data.value("d").toString(),
                              data.value("bn").toString());
            }
            else if (type == u"user.personal_emotes")
            {
                // Someone changed their personal emotes: the whole current
                // list, used from their next message on (and in our own
                // emote popup and completion when it's us). With presets
                // assigned to channels ("ch"), each channel is asked again.
                if (data.value("ch").toBool())
                {
                    this->refreshPersonal(login);
                    return;
                }
                QStringList keys{cacheKey(login, {})};
                {
                    const std::scoped_lock lock(this->mutex_);
                    for (const auto &[key, p] : this->personal_)
                    {
                        if (keyLogin(key) == login && key != keys.front())
                        {
                            keys.append(key);
                        }
                    }
                }
                this->storePersonalEmotes(
                    keys,
                    QJsonObject{
                        {"users", QJsonObject{{login, data.value("emotes")}}},
                        {"cdn", data.value("cdn")},
                    });
            }
        });
}

void Jiltv::setWorn(const QString &login, const QString &paint,
                    const QString &badge, const std::optional<QString> &frame,
                    const std::optional<QString> &banner)
{
    if (login.isEmpty())
    {
        return;
    }
    // The global look: without outfits it is the same in every channel.
    const auto lower = login.toLower();
    bool changed = false;
    bool badgeChanged = false;
    {
        const std::scoped_lock lock(this->mutex_);
        QStringList keys{cacheKey(lower, {})};
        for (const auto &[key, w] : this->worn_)
        {
            if (keyLogin(key) == lower && key != keys.front())
            {
                keys.append(key);
            }
        }
        for (const auto &key : keys)
        {
            changed |=
                this->storeWorn(key, paint, badge, frame, banner, badgeChanged);
        }
    }
    if (changed)
    {
        getApp()->getWindows()->invalidateChannelViewBuffers();
    }
}

void Jiltv::onChannelEvent(const QString &roomId, const QString &type,
                           const QJsonObject &data)
{
    if (type == u"presence.joined")
    {
        // A JilTV user arrived: know their cosmetics and personal emotes
        // before they write.
        // What they bring is what they wear in this channel (an outfit,
        // a preset assigned to it).
        const auto login = data.value("login").toString().toLower();
        const auto channel =
            getApp()->getTwitch()->getChannelOrEmptyByID(roomId)->getName();
        if (login.isEmpty() || channel.isEmpty())
        {
            return;
        }
        const auto key = cacheKey(login, channel);
        const auto cosmetics = data.value("cosmetics").toObject();
        bool changed = false;
        bool badgeChanged = false;
        {
            const std::scoped_lock lock(this->mutex_);
            changed =
                this->storeWorn(key, cosmetics.value("p").toString(),
                                cosmetics.value("b").toString(),
                                cosmetics.value("d").toString(),
                                cosmetics.value("bn").toString(), badgeChanged);
        }
        if (changed)
        {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        }
        if (data.contains("personal_emotes"))
        {
            this->storePersonalEmotes(
                {key},
                QJsonObject{
                    {"users",
                     QJsonObject{{login, data.value("personal_emotes")}}},
                    {"cdn", data.value("cdn")},
                });
        }
        return;
    }
    if (!type.startsWith(u"set.") && type != u"channel.set_switched" &&
        type != u"emote.updated" && type != u"resync")
    {
        return;
    }

    // Say in chat what changed, with the same messages as 7TV and
    // BetterTTV. Names and images of removed and renamed emotes are taken
    // before the set is reloaded, while they are still in it.
    const auto current = this->channelEmotes(roomId);
    const auto emoteOf = [&current](const QString &id) -> EmotePtr {
        if (current)
        {
            for (const auto &[n, e] : *current)
            {
                if (e->id.string == id)
                {
                    return e;
                }
            }
        }
        return nullptr;
    };
    const auto nameOf = [&emoteOf](const QString &id) {
        const auto emote = emoteOf(id);
        return emote ? emote->name.string : id;
    };
    const auto actor = data.value("actor").toObject().value("name").toString();

    struct Change {
        enum class Kind : std::uint8_t { Added, Removed, Renamed };
        Kind kind;
        QString name;
        QString oldName;
        EmotePtr old;
    };
    std::vector<Change> changes;
    const auto removed = [&](const QString &id) {
        changes.push_back({
            .kind = Change::Kind::Removed,
            .name = nameOf(id),
            .oldName = {},
            .old = emoteOf(id),
        });
    };
    const auto renamed = [&](const QString &id, const QString &name) {
        changes.push_back({
            .kind = Change::Kind::Renamed,
            .name = name,
            .oldName = nameOf(id),
            .old = emoteOf(id),
        });
    };
    const auto added = [&](const QString &name) {
        changes.push_back({
            .kind = Change::Kind::Added,
            .name = name,
            .oldName = {},
            .old = nullptr,
        });
    };

    bool switched = false;
    if (type == u"set.batch")
    {
        // Several changes at once (the website's set editor sends these):
        // removals first, then renames, then additions
        for (const auto &id : data.value("removed").toArray())
        {
            removed(id.toString());
        }
        for (const auto &r : data.value("renamed").toArray())
        {
            const auto o = r.toObject();
            renamed(o.value("emote_id").toString(), o.value("name").toString());
        }
        for (const auto &e : data.value("added").toArray())
        {
            added(e.toObject().value("name").toString());
        }
    }
    else if (type == u"set.emote_added")
    {
        added(data.value("emote").toObject().value("name").toString());
    }
    else if (type == u"set.emote_removed")
    {
        removed(data.value("emote_id").toString());
    }
    else if (type == u"set.emote_renamed")
    {
        renamed(data.value("emote_id").toString(),
                data.value("name").toString());
    }
    else if (type == u"channel.set_switched")
    {
        switched = true;
    }

    this->loadChannelEmotes(roomId, [roomId, actor, changes, switched](int) {
        auto channel = getApp()->getTwitch()->getChannelOrEmptyByID(roomId);
        auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
        if (twitch == nullptr)
        {
            return;
        }
        const auto now = Jiltv::instance().channelEmotes(roomId);
        const auto emoteNamed = [&now](const QString &name) -> EmotePtr {
            if (!now)
            {
                return nullptr;
            }
            const auto it = now->find(EmoteName{name});
            return it == now->end() ? nullptr : it->second;
        };
        const auto liveEmote = [](const EmotePtr &emote, const QString &name) {
            if (emote)
            {
                return LiveUpdateEmote{emote};
            }
            LiveUpdateEmote e;
            e.name = name;
            return e;
        };

        for (const auto &change : changes)
        {
            switch (change.kind)
            {
                case Change::Kind::Added:
                    twitch->addJiltvEmoteUpdate(
                        true, actor,
                        liveEmote(emoteNamed(change.name), change.name));
                    break;
                case Change::Kind::Removed:
                    twitch->addJiltvEmoteUpdate(
                        false, actor, liveEmote(change.old, change.name));
                    break;
                case Change::Kind::Renamed: {
                    auto emote = emoteNamed(change.name);
                    twitch->addMessage(
                        MessageBuilder(liveUpdatesUpdateEmoteMessage,
                                       QStringLiteral("JilTV"), actor,
                                       change.name, change.oldName,
                                       emote ? emote : change.old)
                            .release(),
                        MessageContext::Original);
                }
                break;
            }
        }
        if (switched)
        {
            // The new set's name comes with the bootstrap just reloaded
            const auto setName = Jiltv::instance().channelSetName(roomId);
            if (!actor.isEmpty() && !setName.isEmpty())
            {
                twitch->addMessage(
                    MessageBuilder(liveUpdatesUpdateEmoteSetMessage,
                                   QStringLiteral("JilTV"), actor, setName)
                        .release(),
                    MessageContext::Original);
            }
            else if (!setName.isEmpty())
            {
                twitch->addSystemMessage(
                    u"The active JilTV Emote Set was switched to \"%1\"."_s.arg(
                        setName));
            }
            else
            {
                twitch->addSystemMessage(
                    actor.isEmpty()
                        ? QStringLiteral("The JilTV Emote Set was turned off.")
                        : actor + QStringLiteral(
                                      " turned off the JilTV Emote Set."));
            }
        }
    });
}

bool jiltvFirst()
{
    return getSettings()->enableJiltv && getSettings()->jiltvPriority;
}

}  // namespace chatterino
