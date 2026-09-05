// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QJsonObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// A single badge definition from the Bluzyrino registry catalog.
struct BluzyrinoBadge {
    QString id;
    QString category;  // "founder", "donor" or "special"
    EmotePtr emote;
};

/// Fetches and holds Bluzyrino badges from the badge registry.
///
/// The registry exposes a catalog of badges, a per-badge list of user IDs,
/// each user's set of available badges, and — for the donor category — the
/// donor tier the user has chosen to display. From that we compute, per user,
/// the ordered list of badges to show in chat: founder (if any), the selected
/// donor tier, then any special badges, in catalog order.
class BluzyrinoBadges
{
public:
    BluzyrinoBadges();

    void startLoading();
    void loadBluzyrinoBadges();

    /// Badges to display in chat for the given user, in render order.
    std::vector<EmotePtr> getBadges(const UserId &id) const;

    /// The full catalog, in registry order. Used by the "manage my badges"
    /// dialog.
    std::vector<BluzyrinoBadge> catalog() const;

    /// The badge IDs the given user is entitled to display, in catalog order.
    std::vector<QString> availableBadgeIds(const QString &userId) const;

    /// The donor tier the given user currently displays (empty if none).
    QString selectedDonor(const QString &userId) const;

    /// Whether the given user has any founder badge in the catalog.
    bool ownsFounder(const QString &userId) const;

    /// Set the current Twitch account's displayed donor tier on the registry.
    /// `callback` is invoked on the GUI thread with success/failure. On success
    /// the local catalog is refreshed so chat reflects the change.
    void setDonorSelection(const QString &badgeId,
                           std::function<void(bool)> callback = {});

    /// Set whether the current Twitch account's founder badge is shown to
    /// others.
    void setFounderVisible(bool visible,
                           std::function<void(bool)> callback = {});

private:
    void applyJson(const QJsonObject &root);
    void queueRefresh();

    std::atomic_bool loadStarted_{false};
    std::atomic_bool hasLoaded_{false};

    /// Periodically re-fetches the registry so badge changes appear without a
    /// manual reload. Started once from startLoading().
    QTimer refreshTimer_;

    mutable std::shared_mutex mutex_;

    /// Catalog in registry order, guarded by mutex_.
    std::vector<BluzyrinoBadge> catalog_;
    /// Badge id -> catalog index, guarded by mutex_.
    std::unordered_map<QString, size_t> catalogIndex_;
    /// User id -> badge ids the user owns, guarded by mutex_.
    std::unordered_map<QString, std::vector<QString>> userBadges_;
    /// User id -> selected donor badge id, guarded by mutex_.
    std::unordered_map<QString, QString> donorSelection_;
    /// User id -> precomputed render list, guarded by mutex_.
    std::unordered_map<QString, std::vector<EmotePtr>> renderCache_;
};

}  // namespace chatterino
