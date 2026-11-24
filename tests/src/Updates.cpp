// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Version.hpp"
#include "Test.hpp"

#include <semver/semver.hpp>

using namespace chatterino;
using namespace Qt::Literals;

TEST(Updates, MustBeDowngrade)
{
    EXPECT_TRUE(Updates::isDowngradeOf("1.0.0", "2.4.5"))
        << "1.0.0 must be a downgrade of 2.4.5";
    EXPECT_TRUE(Updates::isDowngradeOf("2.0.0", "2.4.5"))
        << "2.0.0 must be a downgrade of 2.4.5";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.0", "2.4.5"))
        << "2.4.0 must be a downgrade of 2.4.5";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.4-beta", "2.4.5"))
        << "2.4.4-beta must be a downgrade of 2.4.5";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.5-beta", "2.4.5"))
        << "2.4.5-beta must be a downgrade of 2.4.5";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.5-beta.1", "2.4.5-beta.2"))
        << "2.4.5-beta.1 must be a downgrade of 2.4.5-beta.2";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.5-beta", "2.4.5-beta.2"))
        << "2.4.5-beta must be a downgrade of 2.4.5-beta.2";
    EXPECT_TRUE(Updates::isDowngradeOf("2.4.5-beta.2", "2.4.6-beta.1"))
        << "2.4.5-beta.2 must be a downgrade of 2.4.6-beta.1";
    EXPECT_TRUE(Updates::isDowngradeOf("2.5.4", "2.5.5-beta.1"))
        << "2.5.4 must be a downgrade of 2.5.5-beta.1";
}

TEST(Updates, MustNotBeDowngrade)
{
    EXPECT_FALSE(Updates::isDowngradeOf("2.4.5", "2.4.5"))
        << "2.4.5 must not be a downgrade of 2.4.5";
    EXPECT_FALSE(Updates::isDowngradeOf("2.4.5", "2.4.5-beta"))
        << "2.4.5 must not be a downgrade of 2.4.5-beta";
}

TEST(Updates, ValidateCurrentVersion)
{
    EXPECT_NO_THROW([[maybe_unused]] auto v = semver::from_string(
                        Version::instance().version().toStdString()))
        << "Current version must be valid semver";
    EXPECT_EQ(Version::instance().version(), CHATTERINO_VERSION);
}

TEST(Updates, MergeChangesOfSkippedReleases)
{
    const auto changes = Updates::mergeChanges({
        u"### Highlights\n\n- New Raid shoutouts. Detail.\n- Fixed Scrolling."_s,
        u"- New Raid shoutouts. Detail.\n- Changed Paints. Less CPU."_s,
    });
    ASSERT_EQ(changes.size(), 3U);
    EXPECT_EQ(changes[0].kind, u"new"_s);
    EXPECT_EQ(changes[0].title, u"Raid shoutouts"_s);
    EXPECT_EQ(changes[1].kind, u"fixed"_s);
    EXPECT_EQ(changes[2].kind, u"changed"_s);
    EXPECT_EQ(changes[2].title, u"Paints"_s);
}
