// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/limerino/LimerinoBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSize>
#include <QUrl>

#include <algorithm>
#include <mutex>
#include <utility>

namespace chatterino {
namespace {

constexpr auto BADGES_URL = "https://api.limerino.com/v1/badges/all";
constexpr auto EVENTS_URL = "https://api.limerino.com/v1/badges/events";
/// The full list from the last run, and its ETag, in the misc folder
constexpr auto STORED_LIST_FILE = "limerino-badges.json";
constexpr auto STORED_ETAG_FILE = "limerino-badges.etag";

QString storedPath(const char *name)
{
    return combinePath(getApp()->getPaths().miscDirectory,
                       QString::fromLatin1(name));
}
constexpr qsizetype MAX_PAYLOAD_SIZE = 8 * 1024 * 1024;
constexpr qint64 REFRESH_INTERVAL = 30 * 60 * 1000;
constexpr qint64 RETRY_INTERVAL = 60 * 1000;
constexpr int EVENT_BUFFER_LIMIT = 1024 * 1024;

using Catalog =
    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>>;

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

bool validBadgeId(const QString &id)
{
    if (id.isEmpty() || id.size() > 32 || id.front() == QLatin1Char('-'))
    {
        return false;
    }
    return std::ranges::all_of(id, [](QChar c) {
        const auto code = c.unicode();
        return (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') ||
               code == '-';
    });
}

bool validTooltip(const QString &tooltip)
{
    if (tooltip.isEmpty() || tooltip.size() > 64)
    {
        return false;
    }
    return std::ranges::all_of(tooltip, [](QChar c) {
        const auto code = c.unicode();
        return code >= 0x20 && code != 0x7F;
    });
}

bool validFileName(const QString &name)
{
    if (name.size() < 6 || name.size() > 7 || name.at(1) != QLatin1Char('x') ||
        name.at(2) != QLatin1Char('.'))
    {
        return false;
    }
    const auto size = name.at(0).unicode();
    if (size < '1' || size > '4')
    {
        return false;
    }
    const auto ext = QStringView{name}.mid(3);
    return ext == u"webp" || ext == u"gif" || ext == u"png";
}

int widthForSize(QChar size)
{
    switch (size.unicode())
    {
        case u'1':
            return 18;
        case u'2':
            return 36;
        case u'3':
            return 54;
        case u'4':
            return 72;
        default:
            return 0;
    }
}

bool formatMatches(const QString &name, const QString &format)
{
    if (format == QStringLiteral("WEBP"))
    {
        return name.endsWith(QStringLiteral(".webp"));
    }
    if (format == QStringLiteral("GIF"))
    {
        return name.endsWith(QStringLiteral(".gif"));
    }
    if (format == QStringLiteral("PNG"))
    {
        return name.endsWith(QStringLiteral(".png"));
    }
    return false;
}

QString imageUrl(const QString &hostUrl, const QString &fileName)
{
    if (!validFileName(fileName) || hostUrl.size() > 512 ||
        !hostUrl.startsWith(QStringLiteral("//")) ||
        hostUrl.contains(QStringLiteral("..")) || hostUrl.contains('\\') ||
        hostUrl.contains('@') || hostUrl.contains(' ') ||
        hostUrl.contains('?') || hostUrl.contains('#') || hostUrl.endsWith('/'))
    {
        return {};
    }

    const QUrl url(
        QStringLiteral("https:") + hostUrl + QLatin1Char('/') + fileName,
        QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("https"))
    {
        return {};
    }
    const auto host = url.host();
    if (host != QStringLiteral("limerino.com") &&
        !host.endsWith(QStringLiteral(".limerino.com")))
    {
        return {};
    }
    if (!url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() ||
        url.hasFragment() ||
        !url.path().startsWith(QStringLiteral("/v1/badges/art/")) ||
        !url.path().endsWith(QLatin1Char('/') + fileName))
    {
        return {};
    }
    return url.toString(QUrl::FullyEncoded);
}

QString chosenName(const QJsonObject &file, bool animate, int width)
{
    const auto original = file.value(QStringLiteral("name")).toString();
    auto name = original;
    if (!animate)
    {
        const auto still = file.value(QStringLiteral("static_name")).toString();
        if (!still.isEmpty())
        {
            name = still;
        }
    }
    if (!validFileName(name) || widthForSize(name.at(0)) != width ||
        file.value(QStringLiteral("width")).toInt() != width ||
        file.value(QStringLiteral("height")).toInt() != width)
    {
        return {};
    }
    if (name == original &&
        !formatMatches(name, file.value(QStringLiteral("format")).toString()))
    {
        return {};
    }
    return name;
}

QString selectFile(const QJsonArray &files, int width, bool animate)
{
    for (const auto *format : {"WEBP", "GIF", "PNG"})
    {
        for (const auto &value : files)
        {
            if (!value.isObject())
            {
                continue;
            }
            const auto file = value.toObject();
            if (file.value(QStringLiteral("format")).toString() !=
                QLatin1String(format))
            {
                continue;
            }
            const auto name = chosenName(file, animate, width);
            if (!name.isEmpty())
            {
                return name;
            }
        }
    }
    return {};
}

ImagePtr imageOrEmpty(const QString &url, qreal scale, QSize expectedSize)
{
    if (url.isEmpty())
    {
        return Image::getEmpty();
    }
    return Image::fromUrl(Url{url}, scale, expectedSize);
}

std::shared_ptr<const Emote> makeBadge(const QJsonObject &object, bool animate)
{
    if (object.value(QStringLiteral("kind")).toString() !=
        QStringLiteral("BADGE"))
    {
        return nullptr;
    }
    const auto id = object.value(QStringLiteral("id")).toString();
    const auto data = object.value(QStringLiteral("data")).toObject();
    const auto tooltip = data.value(QStringLiteral("tooltip")).toString();
    if (!validBadgeId(id) ||
        data.value(QStringLiteral("id")).toString() != id ||
        !validTooltip(tooltip))
    {
        return nullptr;
    }

    const auto host = data.value(QStringLiteral("host")).toObject();
    const auto hostUrl = host.value(QStringLiteral("url")).toString();
    const auto files = host.value(QStringLiteral("files")).toArray();
    const auto url1 = imageUrl(hostUrl, selectFile(files, 18, animate));
    if (url1.isEmpty())
    {
        return nullptr;
    }

    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("limerino:") + id},
        .images =
            ImageSet{
                Image::fromUrl(Url{url1}, 1.0, QSize{18, 18}),
                imageOrEmpty(imageUrl(hostUrl, selectFile(files, 36, animate)),
                             0.5, QSize{36, 36}),
                imageOrEmpty(imageUrl(hostUrl, selectFile(files, 72, animate)),
                             0.25, QSize{72, 72}),
            },
        .tooltip = Tooltip{tooltip.toHtmlEscaped()},
        .homePage = Url{QStringLiteral("https://limerino.com")},
        .id = EmoteId{id},
    });
}

std::shared_ptr<const Emote> lookupFirst(const Catalog &catalog,
                                         const QJsonArray &ids)
{
    for (const auto &value : ids)
    {
        if (!value.isString())
        {
            continue;
        }
        const auto found = catalog.find(value.toString());
        if (found != catalog.end())
        {
            return found->second;
        }
    }
    return nullptr;
}

}  // namespace

LimerinoBadges::LimerinoBadges() = default;

LimerinoBadges::~LimerinoBadges()
{
    this->closeEventStream();
}

void LimerinoBadges::initialize()
{
    this->refreshTimer_.setSingleShot(true);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });

    if (auto *gui = qGuiApp)
    {
        QObject::connect(gui, &QGuiApplication::applicationStateChanged, this,
                         [this](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive)
                             {
                                 this->catchUp();
                             }
                         });
    }

    getSettings()->animateEmotes.connect(
        [this](bool animate, auto) {
            this->animate_ = animate;
        },
        this->signalHolder_);

    // Nothing is requested while Limerino badges are turned off. The callback
    // runs immediately, so an already-on setting fetches once startup has
    // settled.
    getSettings()->showBadgesLimerino.connect(
        [this](bool enabled, auto) {
            if (!enabled)
            {
                this->stop();
                return;
            }
            this->loadStoredCopy();
            this->nextAt_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds{3500};
            QTimer::singleShot(3500, this, [this] {
                this->start();
            });
        },
        this->signalHolder_);
}

std::vector<std::shared_ptr<const Emote>> LimerinoBadges::getBadges(
    const UserId &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto found = this->users_.find(userID.string);
    if (found == this->users_.end() || !found->second)
    {
        return {};
    }
    return {found->second};
}

bool LimerinoBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
    {
        return false;
    }
    const auto root = document.object();
    const auto badgeValues = root.value(QStringLiteral("badges"));
    const auto userValues = root.value(QStringLiteral("users"));
    if (!badgeValues.isArray() || !userValues.isObject())
    {
        return false;
    }

    Catalog catalog;
    for (const auto &value : badgeValues.toArray())
    {
        if (!value.isObject())
        {
            continue;
        }
        if (const auto badge = makeBadge(value.toObject(), this->animate_))
        {
            catalog.emplace(badge->id.string, badge);
        }
    }

    Catalog assigned;
    const auto listed = userValues.toObject();
    for (auto it = listed.begin(); it != listed.end(); ++it)
    {
        if (!validUserId(it.key()) || !it.value().isArray())
        {
            continue;
        }
        const auto badge = lookupFirst(catalog, it.value().toArray());
        if (!badge)
        {
            continue;
        }
        if (assigned.size() >= 200000)
        {
            return false;
        }
        assigned.emplace(it.key(), badge);
    }

    std::unique_lock lock(this->mutex_);
    this->catalog_ = std::move(catalog);
    this->users_ = std::move(assigned);
    return true;
}

void LimerinoBadges::applyHolders(const QJsonObject &event)
{
    const auto userValues = event.value(QStringLiteral("users"));
    if (!userValues.isObject())
    {
        return;
    }

    std::unique_lock lock(this->mutex_);
    const auto listed = userValues.toObject();
    for (auto it = listed.begin(); it != listed.end(); ++it)
    {
        if (!validUserId(it.key()) || !it.value().isArray())
        {
            continue;
        }
        const auto badge = lookupFirst(this->catalog_, it.value().toArray());
        this->users_.erase(it.key());
        if (badge)
        {
            this->users_.emplace(it.key(), badge);
        }
    }
    this->etag_.clear();
}

void LimerinoBadges::loadStoredCopy()
{
    if (this->storedCopyLoaded_)
    {
        return;
    }
    this->storedCopyLoaded_ = true;

    QFile list(storedPath(STORED_LIST_FILE));
    if (!list.open(QIODevice::ReadOnly) || list.size() > MAX_PAYLOAD_SIZE)
    {
        return;
    }
    if (!this->applyPayload(list.readAll()))
    {
        return;
    }
    QFile etag(storedPath(STORED_ETAG_FILE));
    if (etag.open(QIODevice::ReadOnly))
    {
        this->etag_ = etag.readAll().trimmed();
    }
    this->badgesUpdated.invoke();
}

void LimerinoBadges::storeCopy(const QByteArray &payload,
                               const QByteArray &etag) const
{
    QSaveFile list(storedPath(STORED_LIST_FILE));
    if (!list.open(QIODevice::WriteOnly) || list.write(payload) < 0 ||
        !list.commit())
    {
        qCWarning(chatterinoApp) << "[Limerino] Could not keep the badge list.";
        return;
    }
    QSaveFile tag(storedPath(STORED_ETAG_FILE));
    if (tag.open(QIODevice::WriteOnly))
    {
        tag.write(etag);
        tag.commit();
    }
}

void LimerinoBadges::start()
{
    if (!getSettings()->showBadgesLimerino)
    {
        return;
    }
    this->streamClosed_ = false;
    this->refresh();
    this->listen();
}

void LimerinoBadges::stop()
{
    this->refreshTimer_.stop();
    this->again_ = false;
    this->closeEventStream();
}

void LimerinoBadges::reload(std::function<void(bool)> onDone)
{
    if (onDone)
    {
        this->pendingCallbacks_.push_back(std::move(onDone));
    }
    this->refresh();
}

void LimerinoBadges::refresh()
{
    if (!getSettings()->showBadgesLimerino)
    {
        this->finishCallbacks(false);
        return;
    }
    if (this->requestInFlight_)
    {
        this->again_ = true;
        return;
    }
    this->requestInFlight_ = true;
    this->refreshTimer_.stop();

    auto request = NetworkRequest(QUrl{QString::fromLatin1(BADGES_URL)})
                       .header("Accept", "application/json")
                       .attribute(QNetworkRequest::CacheLoadControlAttribute,
                                  QNetworkRequest::AlwaysNetwork)
                       .timeout(10000)
                       .caller(this);
    if (!this->etag_.isEmpty())
    {
        request = std::move(request).header("If-None-Match", this->etag_);
    }

    std::move(request)
        .onSuccess([this](const NetworkResult &result) {
            if (result.status() == 304)
            {
                this->noteSuccess();
                return;
            }
            if (!this->applyPayload(result.getData()))
            {
                qCWarning(chatterinoApp)
                    << "[Limerino] Ignoring an invalid badge list.";
                // A bad body is not a rate-limit or outage, so wait out the
                // normal interval instead of backing off.
                this->noteFailure(false);
                return;
            }
            this->etag_ = result.etag();
            this->storeCopy(result.getData(), this->etag_);
            this->noteSuccess();
            this->badgesUpdated.invoke();
        })
        .onError([this](const NetworkResult &result) {
            const auto code = result.status().value_or(0);
            if (code == 304)
            {
                this->noteSuccess();
                return;
            }
            qCWarning(chatterinoApp)
                << "[Limerino] Badge request failed:" << result.formatError();
            // A 400 means the request itself is wrong, so it is not retried
            // on the failure backoff. The regular interval still runs.
            const auto retry = code == 0 || code == 429 || code >= 500;
            this->noteFailure(retry);
        })
        .finally([this] {
            this->requestInFlight_ = false;
            if (!getSettings()->showBadgesLimerino)
            {
                this->again_ = false;
                return;
            }
            if (this->again_ && this->canPatch_)
            {
                this->again_ = false;
                this->refresh();
                return;
            }
            this->again_ = false;
            this->schedule(this->pendingDelay_);
        })
        .execute();
}

void LimerinoBadges::schedule(std::chrono::milliseconds delay)
{
    if (delay.count() < 0)
    {
        delay = std::chrono::milliseconds{0};
    }
    this->nextAt_ = std::chrono::steady_clock::now() + delay;
    this->refreshTimer_.start(static_cast<int>(delay.count()));
}

void LimerinoBadges::finishCallbacks(bool success)
{
    auto callbacks = std::move(this->pendingCallbacks_);
    this->pendingCallbacks_.clear();
    for (const auto &callback : callbacks)
    {
        callback(success);
    }
}

void LimerinoBadges::noteSuccess()
{
    this->finishCallbacks(true);
    this->failures_ = 0;
    this->canPatch_ = true;
    this->pendingDelay_ = std::chrono::milliseconds{REFRESH_INTERVAL};
}

void LimerinoBadges::noteFailure(bool retry)
{
    this->finishCallbacks(false);
    this->canPatch_ = false;
    auto delay = REFRESH_INTERVAL;
    if (retry)
    {
        const auto exponent = std::min(this->failures_, 8);
        delay = std::min(RETRY_INTERVAL << exponent, REFRESH_INTERVAL);
        this->failures_ = std::min(this->failures_ + 1, 8);
        delay += QRandomGenerator::global()->bounded(5001);
    }
    this->pendingDelay_ = std::chrono::milliseconds{delay};
}

void LimerinoBadges::catchUp()
{
    if (!getSettings()->showBadgesLimerino || this->requestInFlight_)
    {
        return;
    }
    if (this->eventReply_ == nullptr && !this->streamClosed_)
    {
        this->listen();
    }
    if (std::chrono::steady_clock::now() >= this->nextAt_)
    {
        this->refresh();
    }
}

void LimerinoBadges::listen()
{
    if (this->streamClosed_ || this->eventReply_ != nullptr ||
        !getSettings()->showBadgesLimerino)
    {
        return;
    }
    if (this->network_ == nullptr)
    {
        this->network_ = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{QUrl{QString::fromLatin1(EVENTS_URL)}};
    request.setRawHeader("Accept", "text/event-stream");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    this->eventReply_ = this->network_->get(request);
    QObject::connect(this->eventReply_, &QNetworkReply::readyRead, this,
                     [this] {
                         this->readEventStream();
                     });
    QObject::connect(this->eventReply_, &QNetworkReply::finished, this, [this] {
        this->onEventFinished();
    });
}

void LimerinoBadges::closeEventStream()
{
    this->streamClosed_ = true;
    if (this->eventReply_ == nullptr)
    {
        return;
    }
    QObject::disconnect(this->eventReply_, nullptr, this, nullptr);
    this->eventReply_->abort();
    this->eventReply_->deleteLater();
    this->eventReply_ = nullptr;
    this->eventBuffer_.clear();
    this->eventType_.clear();
    this->eventData_.clear();
}

void LimerinoBadges::readEventStream()
{
    auto *reply = this->eventReply_;
    if (reply == nullptr)
    {
        return;
    }
    this->eventBuffer_.append(reply->readAll());
    if (this->eventBuffer_.size() > EVENT_BUFFER_LIMIT)
    {
        qCWarning(chatterinoApp)
            << "[Limerino] Event stream overflowed; reconnecting.";
        reply->abort();
        return;
    }
    this->parseEvents();
}

void LimerinoBadges::onEventFinished()
{
    auto *reply = this->eventReply_;
    if (reply == nullptr)
    {
        return;
    }
    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto retryAfter = reply->rawHeader("Retry-After").toInt();
    this->eventReply_ = nullptr;
    reply->deleteLater();
    this->eventBuffer_.clear();
    this->eventType_.clear();
    this->eventData_.clear();

    if (this->streamClosed_ || !getSettings()->showBadgesLimerino)
    {
        return;
    }

    auto delay = this->eventRetryMs_;
    if (status == 429 || status == 503)
    {
        delay = (retryAfter > 0 ? retryAfter : 60) * 1000;
    }
    QTimer::singleShot(delay, this, [this] {
        this->listen();
    });
}

void LimerinoBadges::parseEvents()
{
    while (true)
    {
        const auto newline = this->eventBuffer_.indexOf('\n');
        if (newline < 0)
        {
            return;
        }
        auto line = this->eventBuffer_.left(newline);
        this->eventBuffer_.remove(0, newline + 1);
        if (line.endsWith('\r'))
        {
            line.chop(1);
        }
        if (line.isEmpty())
        {
            this->dispatchEvent();
            continue;
        }
        if (line.startsWith(':'))
        {
            continue;
        }

        const auto colon = line.indexOf(':');
        QByteArray field;
        QByteArray value;
        if (colon < 0)
        {
            field = line;
        }
        else
        {
            field = line.left(colon);
            value = line.mid(colon + 1);
            if (value.startsWith(' '))
            {
                value.remove(0, 1);
            }
        }

        if (field == "event")
        {
            this->eventType_ = value;
        }
        else if (field == "data")
        {
            if (!this->eventData_.isEmpty())
            {
                this->eventData_.append('\n');
            }
            this->eventData_.append(value);
        }
        else if (field == "retry")
        {
            bool ok = false;
            const auto ms = value.toInt(&ok);
            if (ok && ms >= 0)
            {
                this->eventRetryMs_ = ms;
            }
        }
    }
}

void LimerinoBadges::dispatchEvent()
{
    const auto type = std::move(this->eventType_);
    const auto data = std::move(this->eventData_);
    this->eventType_.clear();
    this->eventData_.clear();

    if (type == "sync")
    {
        this->canPatch_ = false;
        this->refresh();
        return;
    }
    if (type != "holders" || data.isEmpty())
    {
        return;
    }
    const auto document = QJsonDocument::fromJson(data);
    if (!document.isObject())
    {
        return;
    }
    this->handleHolders(document.object());
}

void LimerinoBadges::handleHolders(const QJsonObject &event)
{
    if (this->requestInFlight_)
    {
        this->again_ = true;
        return;
    }
    if (!this->canPatch_)
    {
        return;
    }
    this->applyHolders(event);
}

}  // namespace chatterino
