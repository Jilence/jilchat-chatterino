// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/BadgeRegistry.hpp"

#include <QJsonObject>
#include <QString>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// BetterTTV's badges. Like the extension, a user can have two: a staff
/// badge (developer, support volunteer, emote approver, translator) from
/// BetterTTV's list, and the Pro badge their client announces. getBadge()
/// gives the Pro badge, getStaffBadge() the other.
class BttvBadges : public BadgeRegistry
{
public:
    BttvBadges() = default;

    /// Loads the staff badges
    void load();

    std::optional<EmotePtr> getStaffBadge(const UserId &id) const;

protected:
    QString idForBadge(const QJsonObject &badgeJson) const override;
    EmotePtr createBadge(const QString &id,
                         const QJsonObject &badgeJson) const override;

private:
    mutable std::shared_mutex staffMutex_;
    /// Twitch user id -> staff badge
    std::unordered_map<QString, EmotePtr> staffBadges_;
};

}  // namespace chatterino
