// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/homies/HomiesBadges.hpp"

#include "common/Aliases.hpp"
#include "common/Literals.hpp"
#include "Test.hpp"

#include <QString>

using namespace chatterino;
using namespace literals;

TEST(HomiesBadges, BasicInitialization)
{
    HomiesBadges badges;

    auto found = badges.getBadges(u"123456"_s);
    for (const auto &badge : found)
    {
        EXPECT_FALSE(badge);
    }
}

TEST(HomiesBadges, BadgeRetrieval)
{
    HomiesBadges badges;

    for (const auto &userId : {u"nonexistent"_s, u""_s})
    {
        auto found = badges.getBadges(userId);
        for (const auto &badge : found)
        {
            EXPECT_FALSE(badge) << userId;
        }
    }
}
