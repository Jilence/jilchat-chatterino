// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QJsonArray>
#include <QTimer>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class JilChatBadges
{
public:
    JilChatBadges();

    /// @param onDone called once the answer is there, with whether loading
    ///               worked; from any thread
    void loadJilChatBadges(std::function<void(bool)> onDone = {});

    std::vector<EmotePtr> getBadges(const UserId &id) const;

private:
    void applyBadgeJson(const QJsonArray &jsonRoot);

    /// Periodically re-fetches the registry so badge changes appear without a
    /// manual reload. Started once from the constructor.
    QTimer refreshTimer_;

    mutable std::shared_mutex mutex_;

    /**
     * Maps Twitch user IDs to their JilChat badges
     * Guarded by mutex_
     */
    std::unordered_map<QString, std::vector<EmotePtr>> badgeMap_;

    /// The last payload applied, used to skip unchanged refreshes.
    /// Guarded by mutex_
    QJsonArray lastPayload_;
};

}  // namespace chatterino
