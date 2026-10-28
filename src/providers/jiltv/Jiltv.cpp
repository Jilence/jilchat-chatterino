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
#include "providers/jiltv/JiltvPaint.hpp"
#include "providers/jiltv/JiltvStream.hpp"
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

namespace chatterino {

using namespace Qt::Literals;

namespace {

constexpr int LOOKUP_BATCH = 100;
constexpr int LOOKUP_DELAY_MS = 150;
constexpr qint64 WORN_TTL_SECS = 5 * 60;
constexpr int CATALOG_INTERVAL_MS = 10 * 60 * 1000;

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
        auto tooltip = name + QStringLiteral("<br>JilTV ") + kind +
                       QStringLiteral(" Emote");
        const auto original = e.value("original_name").toString();
        if (!original.isEmpty() && original != name)
        {
            tooltip += QStringLiteral("<br>Alias of ") + original;
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
        paints[id] = std::make_shared<JiltvPaint>(
            id, name, p.value("data").toObject(), cdn);
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
        auto tooltip = b.value("name").toString();
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

    {
        const std::scoped_lock lock(this->mutex_);
        this->paints_ = std::move(paints);
        this->badges_ = std::move(badges);
    }
    getApp()->getWindows()->invalidateChannelViewBuffers();
    WindowManager::notifyBadgesUpdated();
}

std::optional<Jiltv::Worn> Jiltv::worn(const QString &login)
{
    if (!getSettings()->enableJiltv || login.isEmpty())
    {
        return std::nullopt;
    }
    const auto key = login.toLower();
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

void Jiltv::flushLookups()
{
    QStringList batch;
    {
        const std::scoped_lock lock(this->mutex_);
        this->flushScheduled_ = false;
        for (auto it = this->pending_.begin();
             it != this->pending_.end() && batch.size() < LOOKUP_BATCH;)
        {
            batch.append(*it);
            this->inFlight_.insert(*it);
            it = this->pending_.erase(it);
        }
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

    const auto finish = [this, batch](const QJsonObject &users, bool ok) {
        bool changed = false;
        {
            const std::scoped_lock lock(this->mutex_);
            const auto now = QDateTime::currentDateTimeUtc();
            for (const auto &login : batch)
            {
                this->inFlight_.remove(login);
                if (!ok)
                {
                    continue;  // asked again the next time it's drawn
                }
                const auto u = users.value(login).toObject();
                Worn w{u.value("p").toString(), u.value("b").toString(), now};
                auto &slot = this->worn_[login];
                changed |= slot.paint != w.paint || slot.badge != w.badge;
                slot = w;
            }
        }
        if (changed)
        {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        }
    };

    NetworkRequest(QUrl(this->apiUrl() +
                        QStringLiteral("/v1/cosmetics/twitch?logins=") +
                        batch.join(u',')))
        .timeout(20000)
        .onSuccess([finish](const NetworkResult &result) {
            finish(result.parseJson().value("users").toObject(), true);
        })
        .onError([finish](const NetworkResult &) {
            finish({}, false);
        })
        .execute();
}

void Jiltv::prefetch(const QString &login)
{
    (void)this->worn(login);
}

std::shared_ptr<Paint> Jiltv::paint(const QString &login)
{
    const auto w = this->worn(login);
    if (!w || w->paint.isEmpty())
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->paints_.find(w->paint);
    return it == this->paints_.end() ? nullptr : it->second;
}

EmotePtr Jiltv::badge(const QString &login)
{
    if (!getSettings()->showBadgesJiltv)
    {
        return nullptr;
    }
    const auto w = this->worn(login);
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
            auto map = std::make_shared<const EmoteMap>(
                parseBootstrap(result.parseJson(), QStringLiteral("Channel")));
            const auto n = static_cast<int>(map->size());
            qCDebug(chatterinoSeventv)
                << "JilTV: channel" << roomId << "has" << n << "emotes";
            {
                const std::scoped_lock lock(this->mutex_);
                this->channels_[roomId] = std::move(map);
            }
            this->follow(roomId);
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
            }
            if (done)
            {
                done(0);
            }
        })
        .execute();
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

void Jiltv::loadPersonalEmotes(const QString &login, std::function<void()> done)
{
    const auto key = login.toLower();
    if (key.isEmpty() || !getSettings()->enableJiltv ||
        !getSettings()->enableJiltvEmotes)
    {
        return;
    }
    QUrl url(this->apiUrl() + QStringLiteral("/v1/personal-emotes/twitch"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("logins"), key);
    url.setQuery(query);
    NetworkRequest(url)
        .timeout(30000)
        .onSuccess(
            [this, key, done = std::move(done)](const NetworkResult &result) {
                const auto root = result.parseJson();
                auto map = std::make_shared<const EmoteMap>(parseEmotes(
                    root.value("users").toObject().value(key).toArray(),
                    root.value("cdn").toString(), QStringLiteral("Personal")));
                {
                    const std::scoped_lock lock(this->mutex_);
                    this->personal_[key] = std::move(map);
                }
                if (done)
                {
                    done();
                }
            })
        .execute();
}

std::shared_ptr<const EmoteMap> Jiltv::personalEmotes(
    const QString &login) const
{
    if (!getSettings()->enableJiltv || !getSettings()->enableJiltvEmotes)
    {
        return nullptr;
    }
    const std::scoped_lock lock(this->mutex_);
    const auto it = this->personal_.find(login.toLower());
    return it == this->personal_.end() ? nullptr : it->second;
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
            if (type == u"user.cosmetics")
            {
                this->setWorn(data.value("login").toString(),
                              data.value("p").toString(),
                              data.value("b").toString());
            }
        });
}

void Jiltv::setWorn(const QString &login, const QString &paint,
                    const QString &badge)
{
    if (login.isEmpty())
    {
        return;
    }
    bool changed = false;
    {
        const std::scoped_lock lock(this->mutex_);
        auto &slot = this->worn_[login.toLower()];
        changed = slot.paint != paint || slot.badge != badge;
        slot = {paint, badge, QDateTime::currentDateTimeUtc()};
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
        // A JilTV user arrived: know their cosmetics before they write.
        const auto cosmetics = data.value("cosmetics").toObject();
        this->setWorn(data.value("login").toString(),
                      cosmetics.value("p").toString(),
                      cosmetics.value("b").toString());
        return;
    }
    if (!type.startsWith(u"set.") && type != u"channel.set_switched" &&
        type != u"emote.updated" && type != u"resync")
    {
        return;
    }

    // Say in chat what changed, like 7TV does; names are looked up before
    // the set is reloaded, so removed emotes still have theirs.
    const auto current = this->channelEmotes(roomId);
    const auto nameOf = [&current](const QString &id) {
        if (current)
        {
            for (const auto &[n, e] : *current)
            {
                if (e->id.string == id)
                {
                    return n.string;
                }
            }
        }
        return id;
    };
    const auto actor = data.value("actor").toObject().value("name").toString();
    const auto by = actor.isEmpty()
                        ? QString{}
                        : QStringLiteral(" (") + actor + QStringLiteral(")");
    QStringList notices;
    if (type == u"set.batch")
    {
        // Several changes at once (the website's set editor sends these).
        QStringList added;
        for (const auto &e : data.value("added").toArray())
        {
            added.append(e.toObject().value("name").toString());
        }
        QStringList removed;
        for (const auto &id : data.value("removed").toArray())
        {
            removed.append(nameOf(id.toString()));
        }
        QStringList renamed;
        for (const auto &r : data.value("renamed").toArray())
        {
            const auto o = r.toObject();
            renamed.append(nameOf(o.value("emote_id").toString()) +
                           QStringLiteral(" to ") + o.value("name").toString());
        }
        if (!added.isEmpty())
        {
            notices.append(QStringLiteral("JilTV: added ") +
                           added.join(QStringLiteral(", ")) + by);
        }
        if (!removed.isEmpty())
        {
            notices.append(QStringLiteral("JilTV: removed ") +
                           removed.join(QStringLiteral(", ")) + by);
        }
        if (!renamed.isEmpty())
        {
            notices.append(QStringLiteral("JilTV: renamed ") +
                           renamed.join(QStringLiteral(", ")) + by);
        }
    }
    else if (type == u"set.emote_added")
    {
        notices.append(QStringLiteral("JilTV: added ") +
                       data.value("emote").toObject().value("name").toString() +
                       by);
    }
    else if (type == u"set.emote_removed")
    {
        notices.append(QStringLiteral("JilTV: removed ") +
                       nameOf(data.value("emote_id").toString()) + by);
    }
    else if (type == u"channel.set_switched")
    {
        notices.append(QStringLiteral("JilTV: switched the emote set") + by);
    }

    this->loadChannelEmotes(roomId, [roomId, notices](int) {
        if (notices.isEmpty())
        {
            return;
        }
        auto channel = getApp()->getTwitch()->getChannelOrEmptyByID(roomId);
        if (channel->isEmpty())
        {
            return;
        }
        for (const auto &notice : notices)
        {
            channel->addSystemMessage(notice);
        }
    });
}

bool jiltvFirst()
{
    return getSettings()->enableJiltv && getSettings()->jiltvPriority;
}

}  // namespace chatterino
