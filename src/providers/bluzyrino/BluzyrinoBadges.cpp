// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bluzyrino/BluzyrinoBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/network/NetworkCommon.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSize>
#include <QStringBuilder>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <mutex>

namespace chatterino {

using namespace Qt::Literals;

namespace {

constexpr auto BLUZYRINO_BADGE_URL =
    "https://bluzyrino-badge-registry.blu901-55.workers.dev/v1/badges";
constexpr QSize BADGE_BASE_SIZE(18, 18);

EmotePtr makeBadgeEmote(const QJsonObject &badgeJson)
{
    const auto url1x = badgeJson.value("image_url_1x").toString();
    const auto url2x = badgeJson.value("image_url_2x").toString();
    const auto url4x = badgeJson.value("image_url_4x").toString();
    if (url1x.isEmpty())
    {
        return nullptr;
    }

    const auto id = badgeJson.value("id").toString();
    const auto tooltip = badgeJson.value("tooltip").toString();

    auto pickUrl = [&](const QString &preferred) {
        return preferred.isEmpty() ? url1x : preferred;
    };

    auto emote = Emote{
        .name = EmoteName{u"bluzyrino:" % (id.isEmpty() ? tooltip : id)},
        .images =
            ImageSet{
                Image::fromUrl(Url{url1x}, 1.0, BADGE_BASE_SIZE),
                Image::fromUrl(Url{pickUrl(url2x)}, 0.5, BADGE_BASE_SIZE * 2),
                Image::fromUrl(Url{pickUrl(url4x)}, 0.25, BADGE_BASE_SIZE * 4),
            },
        .tooltip = Tooltip{tooltip.isEmpty() ? id : tooltip},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

}  // namespace

BluzyrinoBadges::BluzyrinoBadges()
{
    QTimer::singleShot(3500, [this] {
        this->startLoading();
    });
}

void BluzyrinoBadges::startLoading()
{
    if (this->loadStarted_.exchange(true))
    {
        return;
    }

    this->loadBluzyrinoBadges();
}

void BluzyrinoBadges::loadBluzyrinoBadges()
{
    NetworkRequest(QUrl(BLUZYRINO_BADGE_URL))
        .header("Accept", "application/json")
        .timeout(5000)
        .concurrent()
        .onSuccess([this](const NetworkResult &result) {
            const auto root = result.parseJson();
            if (root.isEmpty())
            {
                qCWarning(chatterinoApp)
                    << "[Bluzyrino] Empty/invalid badge payload";
                return;
            }
            this->applyJson(root);
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Bluzyrino] Failed to load badges:" << result.formatError();
        })
        .execute();
}

void BluzyrinoBadges::applyJson(const QJsonObject &root)
{
    std::vector<BluzyrinoBadge> catalog;
    std::unordered_map<QString, size_t> catalogIndex;

    for (const auto &value : root.value("catalog").toArray())
    {
        const auto obj = value.toObject();
        auto emote = makeBadgeEmote(obj);
        if (!emote)
        {
            continue;
        }
        BluzyrinoBadge badge{
            .id = obj.value("id").toString(),
            .category = obj.value("category").toString(),
            .emote = std::move(emote),
        };
        catalogIndex[badge.id] = catalog.size();
        catalog.push_back(std::move(badge));
    }

    // badge id -> list of user ids that own it
    std::unordered_map<QString, std::vector<QString>> userBadges;
    for (const auto &value : root.value("badges").toArray())
    {
        const auto obj = value.toObject();
        const auto badgeId = obj.value("id").toString();
        if (!catalogIndex.contains(badgeId))
        {
            continue;
        }
        for (const auto &userValue : obj.value("users").toArray())
        {
            const auto userId = userValue.toString();
            if (!userId.isEmpty())
            {
                userBadges[userId].push_back(badgeId);
            }
        }
    }

    // user id -> selected donor tier id
    std::unordered_map<QString, QString> donorSelection;
    const auto selections = root.value("selections").toObject();
    for (auto it = selections.begin(); it != selections.end(); ++it)
    {
        const auto donor = it.value().toObject().value("donor").toString();
        if (!donor.isEmpty())
        {
            donorSelection[it.key()] = donor;
        }
    }

    // Precompute the render list per user: founder, then the chosen donor tier,
    // then any special badges — all in catalog order.
    std::unordered_map<QString, std::vector<EmotePtr>> renderCache;
    for (const auto &[userId, ownedIds] : userBadges)
    {
        // Keep owned ids in catalog order for stable rendering.
        auto owned = ownedIds;
        std::sort(owned.begin(), owned.end(),
                  [&](const QString &a, const QString &b) {
                      return catalogIndex.at(a) < catalogIndex.at(b);
                  });

        const auto selectedDonorIt = donorSelection.find(userId);
        QString donorToShow;
        if (selectedDonorIt != donorSelection.end() &&
            std::find(owned.begin(), owned.end(), selectedDonorIt->second) !=
                owned.end())
        {
            donorToShow = selectedDonorIt->second;
        }

        std::vector<EmotePtr> render;
        auto append = [&](const QString &id) {
            render.push_back(catalog.at(catalogIndex.at(id)).emote);
        };

        // founder
        for (const auto &id : owned)
        {
            if (catalog.at(catalogIndex.at(id)).category == u"founder"_s)
            {
                append(id);
            }
        }
        // donor: the selected tier, or — absent a valid selection — the
        // first owned donor tier in catalog order.
        if (donorToShow.isEmpty())
        {
            for (const auto &id : owned)
            {
                if (catalog.at(catalogIndex.at(id)).category == u"donor"_s)
                {
                    donorToShow = id;
                    break;
                }
            }
        }
        if (!donorToShow.isEmpty())
        {
            append(donorToShow);
        }
        // specials
        for (const auto &id : owned)
        {
            if (catalog.at(catalogIndex.at(id)).category == u"special"_s)
            {
                append(id);
            }
        }

        if (!render.empty())
        {
            renderCache[userId] = std::move(render);
        }
    }

    const bool hadData = !catalog.empty();

    {
        std::unique_lock lock(this->mutex_);
        this->catalog_ = std::move(catalog);
        this->catalogIndex_ = std::move(catalogIndex);
        this->userBadges_ = std::move(userBadges);
        this->donorSelection_ = std::move(donorSelection);
        this->renderCache_ = std::move(renderCache);
    }

    if (hadData)
    {
        this->hasLoaded_.store(true);
        this->queueRefresh();
    }

    qCDebug(chatterinoApp)
        << "[Bluzyrino] Loaded" << this->catalog_.size() << "catalog badges";
}

void BluzyrinoBadges::queueRefresh()
{
    QTimer::singleShot(0, [] {
        if (auto *windows = getApp()->getWindows())
        {
            windows->invalidateChannelViewBuffers();
        }
    });
}

std::vector<EmotePtr> BluzyrinoBadges::getBadges(const UserId &id) const
{
    if (!this->hasLoaded_.load())
    {
        return {};
    }

    std::shared_lock lock(this->mutex_);
    const auto it = this->renderCache_.find(id.string);
    if (it != this->renderCache_.end())
    {
        return it->second;
    }
    return {};
}

std::vector<BluzyrinoBadge> BluzyrinoBadges::catalog() const
{
    std::shared_lock lock(this->mutex_);
    return this->catalog_;
}

std::vector<QString> BluzyrinoBadges::availableBadgeIds(
    const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->userBadges_.find(userId);
    if (it == this->userBadges_.end())
    {
        return {};
    }
    auto ids = it->second;
    std::sort(ids.begin(), ids.end(), [&](const QString &a, const QString &b) {
        return this->catalogIndex_.at(a) < this->catalogIndex_.at(b);
    });
    return ids;
}

QString BluzyrinoBadges::selectedDonor(const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->donorSelection_.find(userId);
    return it == this->donorSelection_.end() ? QString{} : it->second;
}

bool BluzyrinoBadges::ownsFounder(const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->userBadges_.find(userId);
    if (it == this->userBadges_.end())
    {
        return false;
    }
    for (const auto &id : it->second)
    {
        const auto ci = this->catalogIndex_.find(id);
        if (ci != this->catalogIndex_.end() &&
            this->catalog_.at(ci->second).category == u"founder"_s)
        {
            return true;
        }
    }
    return false;
}

namespace {

/// Builds a PUT to the registry authenticated as the current Twitch account.
/// Returns false (and does not call the network) if no account is logged in.
bool sendAuthorizedPut(const QString &url, const QJsonObject &body,
                       std::function<void(bool)> callback)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon() || account->getOAuthToken().isEmpty())
    {
        if (callback)
        {
            callback(false);
        }
        return false;
    }

    NetworkRequest(QUrl(url), NetworkRequestType::Put)
        .header("Authorization", u"OAuth "_s % account->getOAuthToken())
        .header("Content-Type", "application/json")
        .header("Accept", "application/json")
        .payload(QJsonDocument(body).toJson(QJsonDocument::Compact))
        .timeout(5000)
        .onSuccess([callback](const NetworkResult &) {
            if (callback)
            {
                callback(true);
            }
        })
        .onError([callback](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Bluzyrino] Write failed:" << result.formatError();
            if (callback)
            {
                callback(false);
            }
        })
        .execute();
    return true;
}

}  // namespace

void BluzyrinoBadges::setDonorSelection(const QString &badgeId,
                                        std::function<void(bool)> callback)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account)
    {
        if (callback)
        {
            callback(false);
        }
        return;
    }
    const QString url = QString(BLUZYRINO_BADGE_URL) % u"/users/"_s %
                        account->getUserId() % u"/donor"_s;
    sendAuthorizedPut(url, QJsonObject{{u"badge"_s, badgeId}},
                      [this, callback](bool ok) {
                          if (ok)
                          {
                              this->loadBluzyrinoBadges();
                          }
                          if (callback)
                          {
                              callback(ok);
                          }
                      });
}

void BluzyrinoBadges::setFounderVisible(bool visible,
                                        std::function<void(bool)> callback)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account)
    {
        if (callback)
        {
            callback(false);
        }
        return;
    }
    const QString url = QString(BLUZYRINO_BADGE_URL) % u"/founder/users/"_s %
                        account->getUserId() % u"/visibility"_s;
    sendAuthorizedPut(url, QJsonObject{{u"visible"_s, visible}},
                      [this, callback](bool ok) {
                          if (ok)
                          {
                              this->loadBluzyrinoBadges();
                          }
                          if (callback)
                          {
                              callback(ok);
                          }
                      });
}

}  // namespace chatterino
