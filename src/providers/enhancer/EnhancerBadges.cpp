// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/enhancer/EnhancerBadges.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace chatterino {

using namespace chatterino::literals;

namespace {

const QString API = u"https://api.enhancer.at/v1"_s;
constexpr QSize BADGE_BASE_SIZE(18, 18);
/// The most the API gives per page
constexpr int PAGE_SIZE = 100;
/// Stops a cursor that never ends from asking forever
constexpr int MAX_PAGES = 200;

/// One load: the badges first, then who shows each of them
struct Loading {
    std::vector<std::pair<QString, EmotePtr>> badges;
    std::unordered_map<QString, std::vector<EmotePtr>> users;
    size_t badge = 0;
    int pages = 0;
    std::function<void(bool)> done;
};

QUrl pageUrl(const QString &path, const QString &cursor)
{
    QUrl url(API + path);
    QUrlQuery query;
    query.addQueryItem(u"limit"_s, QString::number(PAGE_SIZE));
    if (!cursor.isEmpty())
    {
        query.addQueryItem(u"cursor"_s, cursor);
    }
    url.setQuery(query);
    return url;
}

EmotePtr makeBadge(const QJsonObject &badge)
{
    const auto images = badge.value("images").toObject();
    const auto image = [&](const char *size) {
        return images.value(size).toString();
    };
    if (image("1x").isEmpty())
    {
        return nullptr;
    }
    const auto at = [&](const char *size, const char *fallback) {
        const auto url = image(size);
        return url.isEmpty() ? image(fallback) : url;
    };
    const auto name = badge.value("name").toString();
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{u"enhancer:"_s + badge.value("id").toString()},
        .images =
            ImageSet{
                Image::fromUrl(Url{image("1x")}, 1.0, BADGE_BASE_SIZE),
                Image::fromUrl(Url{at("2x", "1x")}, 0.5, BADGE_BASE_SIZE * 2),
                Image::fromUrl(Url{at("4x", "2x")}, 0.25, BADGE_BASE_SIZE * 4),
            },
        .tooltip = Tooltip{name},
        .homePage = Url{},
        .id = EmoteId{badge.value("id").toString()},
    });
}

void fail(const std::shared_ptr<Loading> &loading, bool *flag,
          const NetworkResult &result)
{
    qCWarning(chatterinoApp)
        << "[Enhancer] Failed to load badges:" << result.formatError();
    *flag = false;
    if (loading->done)
    {
        loading->done(false);
    }
}

}  // namespace

EnhancerBadges::EnhancerBadges()
{
    QTimer::singleShot(4000, [this] {
        this->load();
    });
}

std::vector<EmotePtr> EnhancerBadges::getBadges(const QString &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->badgeMap_.find(userID);
    return it != this->badgeMap_.end() ? it->second : std::vector<EmotePtr>{};
}

void EnhancerBadges::load(const std::function<void(bool)> &onDone)
{
    if (this->loading_)
    {
        if (onDone)
        {
            onDone(false);
        }
        return;
    }
    this->loading_ = true;
    auto loading = std::make_shared<Loading>();
    loading->done = onDone;

    // Who shows the badge at `loading->badge`, page by page; then the next
    // badge, and at the end the result replaces the badges loaded before
    auto fetchUsers = std::make_shared<std::function<void(QString)>>();
    *fetchUsers = [this, loading, fetchUsers](const QString &cursor) {
        if (loading->badge >= loading->badges.size())
        {
            const auto count = loading->users.size();
            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(loading->users);
            }
            this->loading_ = false;
            WindowManager::notifyBadgesUpdated();
            qCDebug(chatterinoApp)
                << "[Enhancer] Loaded badges for" << count << "users";
            if (loading->done)
            {
                loading->done(true);
            }
            // Last, as it ends this function: breaks the reference cycle
            auto self = fetchUsers;
            *self = nullptr;
            return;
        }
        const auto &id = loading->badges[loading->badge].first;
        NetworkRequest(pageUrl(u"/badge/"_s + id + u"/users"_s, cursor))
            .timeout(15000)
            .onSuccess([loading, fetchUsers](const NetworkResult &result) {
                const auto root = result.parseJson();
                const auto &badge = loading->badges[loading->badge].second;
                for (const auto &value : root.value("users").toArray())
                {
                    const auto user = value.toObject();
                    if (user.value("platform").toString() == u"twitch")
                    {
                        loading->users[user.value("id").toString()].push_back(
                            badge);
                    }
                }
                const auto next = root.value("nextCursor").toString();
                if (next.isEmpty() || ++loading->pages >= MAX_PAGES)
                {
                    ++loading->badge;
                    loading->pages = 0;
                    (*fetchUsers)({});
                }
                else
                {
                    (*fetchUsers)(next);
                }
            })
            .onError([this, loading, fetchUsers](const NetworkResult &result) {
                *fetchUsers = nullptr;
                fail(loading, &this->loading_, result);
            })
            .execute();
    };

    // The global badges, page by page
    auto fetchBadges = std::make_shared<std::function<void(QString)>>();
    *fetchBadges = [this, loading, fetchBadges,
                    fetchUsers](const QString &cursor) {
        NetworkRequest(pageUrl(u"/channel/twitch/global/badges"_s, cursor))
            .timeout(15000)
            .onSuccess([loading, fetchBadges,
                        fetchUsers](const NetworkResult &result) {
                const auto root = result.parseJson();
                for (const auto &value : root.value("badges").toArray())
                {
                    const auto badge = value.toObject();
                    // Nobody shows it: no need to ask who does
                    if (badge.value("users").toInt() <= 0)
                    {
                        continue;
                    }
                    if (auto emote = makeBadge(badge))
                    {
                        loading->badges.emplace_back(
                            badge.value("id").toString(), std::move(emote));
                    }
                }
                const auto next = root.value("nextCursor").toString();
                if (next.isEmpty() || ++loading->pages >= MAX_PAGES)
                {
                    loading->pages = 0;
                    *fetchBadges = nullptr;  // breaks the reference cycle
                    (*fetchUsers)({});
                }
                else
                {
                    (*fetchBadges)(next);
                }
            })
            .onError([this, loading, fetchBadges,
                      fetchUsers](const NetworkResult &result) {
                *fetchBadges = nullptr;
                *fetchUsers = nullptr;
                fail(loading, &this->loading_, result);
            })
            .execute();
    };
    (*fetchBadges)({});
}

}  // namespace chatterino
