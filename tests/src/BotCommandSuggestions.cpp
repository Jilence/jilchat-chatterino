// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "providers/potat/PotatCommands.hpp"
#include "providers/supibot/SupibotCommands.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

using namespace chatterino;
using namespace chatterino::completion;

namespace {

QStringList invocations(const std::vector<CommandItem> &items)
{
    QStringList result;
    for (const auto &item : items)
    {
        result.push_back(item.prefix + item.name);
    }
    return result;
}

std::vector<CommandItem> apply(const std::vector<CommandItem> &items,
                               const QString &query, bool startsWithOnly)
{
    CommandStrategy strategy(startsWithOnly);
    std::vector<CommandItem> output;
    strategy.apply(items, output, query);
    return output;
}

QJsonObject channelPayload(const QString &state, const QJsonArray &prefixes,
                           const QJsonArray &commands)
{
    return {
        {QStringLiteral("data"),
         QJsonArray{QJsonObject{
             {QStringLiteral("channel"),
              QJsonObject{
                  {QStringLiteral("state"), state},
                  {QStringLiteral("settings"),
                   QJsonObject{{QStringLiteral("prefix"), prefixes}}},
                  {QStringLiteral("commands"), commands},
              }},
         }}},
    };
}

QByteArray payloadBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

}  // namespace

TEST(SupibotSuggestions, userAliasKeepsADistinctPrefix)
{
    const auto payload = payloadBytes(QJsonObject{
        {QStringLiteral("statusCode"), 200},
        {QStringLiteral("data"),
         QJsonArray{QJsonObject{
             {QStringLiteral("name"), QStringLiteral("ping")},
             {QStringLiteral("description"), QStringLiteral("my alias")},
         }}},
    });
    bool ok = false;
    const auto aliases = supibot::detail::parseUserAliases(payload, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(aliases.size(), 1);
    EXPECT_TRUE(aliases.front().userAlias);
    EXPECT_FALSE(aliases.front().alias);
    EXPECT_EQ(aliases.front().name, QStringLiteral("ping"));

    const auto merged =
        supibot::detail::mergeCommands({SupibotCommand{
                                           .name = QStringLiteral("ping"),
                                           .usage = QStringLiteral("built-in"),
                                           .alias = false,
                                       }},
                                       aliases);
    ASSERT_EQ(merged.size(), 2);
    EXPECT_FALSE(merged.at(0).userAlias);
    EXPECT_TRUE(merged.at(1).userAlias);
}

TEST(CommandStrategySuggestions, dollarPrefixesStaySeparate)
{
    const std::vector<CommandItem> items{
        {.name = QStringLiteral("ping"), .prefix = QStringLiteral("$")},
        {.name = QStringLiteral("ping"), .prefix = QStringLiteral("$$")},
        {.name = QStringLiteral("alias"), .prefix = QStringLiteral("$$")},
    };

    EXPECT_EQ(invocations(apply(items, QStringLiteral("$p"), false)),
              QStringList{QStringLiteral("$ping")});
    EXPECT_EQ(
        invocations(apply(items, QStringLiteral("$$"), true)),
        (QStringList{QStringLiteral("$$ping"), QStringLiteral("$$alias")}));
    EXPECT_EQ(invocations(apply(items, QStringLiteral("$$a"), false)),
              QStringList{QStringLiteral("$$alias")});
}

TEST(CommandStrategySuggestions, slashCommandsStillMatch)
{
    const std::vector<CommandItem> items{
        {.name = QStringLiteral("ban"), .prefix = QStringLiteral("/")},
        {.name = QStringLiteral("potato"), .prefix = QStringLiteral("#")},
    };

    EXPECT_EQ(invocations(apply(items, QStringLiteral("/ba"), true)),
              QStringList{QStringLiteral("/ban")});
    EXPECT_EQ(invocations(apply(items, QStringLiteral("ban"), false)),
              QStringList{QStringLiteral("/ban")});
}

TEST(PotatSuggestions, channelPrefixesAndCustomCommands)
{
    const auto commands = QJsonArray{
        QJsonObject{
            {QStringLiteral("active"), true},
            {QStringLiteral("trigger"), QStringLiteral("^!uwu")},
            {QStringLiteral("help"), QStringLiteral("be cute")},
        },
        QJsonObject{
            {QStringLiteral("active"), true},
            {QStringLiteral("trigger"), QStringLiteral("")},
            {QStringLiteral("name"), QStringLiteral("{any}discord")},
            {QStringLiteral("help"), QStringLiteral("server")},
        },
        QJsonObject{
            {QStringLiteral("active"), true},
            {QStringLiteral("trigger"), QStringLiteral("/./")},
            {QStringLiteral("name"), QStringLiteral("regex")},
        },
        QJsonObject{
            {QStringLiteral("active"), true},
            {QStringLiteral("trigger"), QStringLiteral("^secret$")},
            {QStringLiteral("user_trigger_ids"), QJsonArray{1}},
        },
        QJsonObject{
            {QStringLiteral("active"), false},
            {QStringLiteral("trigger"), QStringLiteral("^asleep$")},
        },
    };
    const auto parsed = potat::detail::parseChannel(payloadBytes(channelPayload(
        QStringLiteral("JOINED"),
        QJsonArray{QStringLiteral("#"), QStringLiteral("-")}, commands)));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->joined);
    EXPECT_EQ(parsed->prefixes,
              (QStringList{QStringLiteral("#"), QStringLiteral("-")}));

    QStringList custom;
    for (const auto &command : parsed->commands)
    {
        custom.push_back(command.prefix + command.name);
    }
    EXPECT_EQ(custom,
              (QStringList{QStringLiteral("!uwu"), QStringLiteral("#discord"),
                           QStringLiteral("-discord")}));
    EXPECT_EQ(parsed->commands.front().usage, QStringLiteral("be cute"));
}

TEST(PotatSuggestions, missingBotProducesNoCommands)
{
    const auto parted = potat::detail::parseChannel(payloadBytes(channelPayload(
        QStringLiteral("PARTED"), QJsonArray{QStringLiteral("#")}, {})));
    ASSERT_TRUE(parted.has_value());
    EXPECT_FALSE(parted->joined);
    EXPECT_TRUE(parted->commands.empty());

    const auto missing = potat::detail::parseChannel(
        payloadBytes(QJsonObject{{QStringLiteral("data"), QJsonArray{}}}));
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->joined);

    EXPECT_FALSE(
        potat::detail::parseChannel(QByteArray("not json")).has_value());
}

TEST(CommandStrategySuggestions, potatPrefixDoesNotCross)
{
    const std::vector<CommandItem> items{
        {.name = QStringLiteral("potato"), .prefix = QStringLiteral("#")},
        {.name = QStringLiteral("potato"), .prefix = QStringLiteral("-")},
        {.name = QStringLiteral(""), .prefix = QStringLiteral("!")},
        {.name = QStringLiteral(""), .prefix = QStringLiteral("!uwu")},
    };

    EXPECT_EQ(invocations(apply(items, QStringLiteral("-po"), false)),
              QStringList{QStringLiteral("-potato")});
    EXPECT_EQ(invocations(apply(items, QStringLiteral("!u"), true)),
              QStringList{QStringLiteral("!uwu")});
    EXPECT_EQ(invocations(apply(items, QStringLiteral("!"), true)),
              (QStringList{QStringLiteral("!"), QStringLiteral("!uwu")}));
}
