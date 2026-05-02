// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QJsonArray>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#if __has_include(<gtest/gtest_prod.h>)
#    include <gtest/gtest_prod.h>
#endif

#ifdef FRIEND_TEST
class FfzApBadges_Supporters_Test;
#endif

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class FfzApBadges
{
public:
    FfzApBadges();

    void loadFfzApBadges();

    std::optional<EmotePtr> getBadge(const UserId &id) const;

private:
    void applySupportersJson(const QJsonArray &jsonRoot);

    mutable std::shared_mutex mutex_;

    /**
     * Maps Twitch user IDs to their FFZ:AP badge
     * Guarded by mutex_
     */
    std::unordered_map<QString, EmotePtr> badgeMap_;

#ifdef FRIEND_TEST
    FRIEND_TEST(FfzApBadges, Supporters);
    friend class ::FfzApBadges_Supporters_Test;
#endif
};

}  // namespace chatterino
