// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/ffzap/FfzApBadges.hpp"

#include "messages/Emote.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

using namespace chatterino;

TEST(FfzApBadges, Supporters)
{
    FfzApBadges badges;

    QJsonArray root;
    root.append(QJsonObject{
        {"id", "1392267890"},
        {"tier", 1},
    });
    root.append(QJsonObject{
        {"id", ""},
        {"tier", 3},
    });

    badges.applySupportersJson(root);

    const auto badge = badges.getBadge({u"1392267890"});
    ASSERT_TRUE(badge.has_value());
    EXPECT_EQ((*badge)->name.string, QStringLiteral("ffzap:supporter"));
    EXPECT_EQ((*badge)->tooltip.string,
              QStringLiteral("FFZ:AP Supporter Tier 1"));
    ASSERT_TRUE((*badge)->images.getImage1());
    EXPECT_EQ((*badge)->images.getImage1()->url().string,
              QStringLiteral(
                  "https://api.ffzap.com/v1/user/badge/1392267890/2"));

    EXPECT_FALSE(badges.getBadge({u""}).has_value());
    EXPECT_FALSE(badges.getBadge({u"123"}).has_value());
}
