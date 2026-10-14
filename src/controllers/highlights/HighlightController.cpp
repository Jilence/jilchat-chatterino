// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"

#include <algorithm>

namespace {

using namespace chatterino;

auto highlightPhraseCheck(const HighlightPhrase &highlight) -> HighlightCheck
{
    return HighlightCheck{
        [highlight](
            const auto &args, const auto &twitchBadges, const auto &senderName,
            const auto &originalMessage, const auto &flags, const auto self,
            const auto platform, const auto &channelName,
            const auto collectMatches) -> std::optional<HighlightResult> {
            (void)args;
            (void)twitchBadges;
            (void)senderName;
            (void)flags;

            if (self || !highlight.getChannelScope().appliesToNormalized(
                            platform, channelName))
            {
                return std::nullopt;
            }

            if (!highlight.isMatch(originalMessage))
            {
                return std::nullopt;
            }

            std::optional<QUrl> highlightSoundUrl;
            if (highlight.hasCustomSound())
            {
                highlightSoundUrl = highlight.getSoundUrl();
            }

            auto result = HighlightResult{
                highlight.hasAlert(),       highlight.hasSound(),
                highlightSoundUrl,          highlight.getColor(),
                highlight.showInMentions(),
            };
            if (collectMatches &&
                highlight.getMatchStyle() != HighlightMatchStyle::None)
            {
                for (const auto &[start, length] :
                     highlight.findMatches(originalMessage))
                {
                    result.matches.push_back({
                        .start = start,
                        .length = length,
                        .color = *highlight.getMatchColor(),
                        .ruleName = highlight.getPattern(),
                        .pattern = highlight.getPattern(),
                        .source = HighlightMatchSource::Phrase,
                        .style = highlight.getMatchStyle(),
                        .paintID = {},
                    });
                }
            }
            return result;
        }};
}

void rebuildSubscriptionHighlights(Settings &settings,
                                   std::vector<HighlightCheck> &checks)
{
    if (settings.enableSubHighlight)
    {
        auto highlightSound = settings.enableSubHighlightSound.getValue();
        auto highlightAlert = settings.enableSubHighlightTaskbar.getValue();
        auto highlightSoundUrlValue = settings.subHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isSubscriptionMessage)
                {
                    return std::nullopt;
                }

                auto highlightColor =
                    ColorProvider::instance().color(ColorType::Subscription);

                return HighlightResult{
                    highlightAlert, highlightSound, highlightSoundUrl,
                    highlightColor, false,
                };
            }});
    }
}

void rebuildFollowHighlights(Settings &settings,
                             std::vector<HighlightCheck> &checks)
{
    if (settings.enableFollowHighlight)
    {
        auto highlightSound = settings.enableFollowHighlightSound.getValue();
        auto highlightAlert = settings.enableFollowHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.followHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)self;

                if (!flags.has(MessageFlag::Follow))
                {
                    return std::nullopt;
                }

                auto highlightColor =
                    ColorProvider::instance().color(ColorType::Follow);

                return HighlightResult{
                    highlightAlert, highlightSound, highlightSoundUrl,
                    highlightColor, false,
                };
            }});
    }
}

void rebuildWhisperHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks)
{
    if (settings.enableWhisperHighlight)
    {
        auto highlightSound = settings.enableWhisperHighlightSound.getValue();
        auto highlightAlert = settings.enableWhisperHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.whisperHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isReceivedWhisper)
                {
                    return std::nullopt;
                }

                return HighlightResult{
                    highlightAlert,
                    highlightSound,
                    highlightSoundUrl,
                    ColorProvider::instance().color(ColorType::Whisper),
                    false,
                };
            }});
    }
}

void rebuildReplyThreadHighlight(Settings &settings,
                                 std::vector<HighlightCheck> &checks)
{
    if (settings.enableThreadHighlight)
    {
        auto highlightSound = settings.enableThreadHighlightSound.getValue();
        auto highlightAlert = settings.enableThreadHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.threadHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }
        auto highlightInMentions =
            settings.showThreadHighlightInMentions.getValue();
        checks.emplace_back(HighlightCheck{
            [=](const auto &, const auto &, const auto &, const auto &,
                const auto &flags, const auto self, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                if (flags.has(MessageFlag::SubscribedThread) && !self)
                {
                    return HighlightResult{
                        highlightAlert,
                        highlightSound,
                        highlightSoundUrl,
                        ColorProvider::instance().color(
                            ColorType::ThreadMessageHighlight),
                        highlightInMentions,
                    };
                }

                return std::nullopt;
            }});
    }
}

void rebuildMessageHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks)
{
    // how the own name is marked where it is mentioned
    auto selfMatchColor = QColor(settings.selfHighlightMatchColor.getValue());
    if (!selfMatchColor.isValid())
    {
        selfMatchColor = defaultNewHighlightMatchColor();
    }
    const auto selfMatchStyle = highlightMatchStyleFromName(
        settings.selfHighlightMatchStyle.getValue());

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    QString currentUsername = currentUser->getUserName();

    if (settings.enableSelfHighlight && !currentUsername.isEmpty() &&
        !currentUser->isAnon())
    {
        HighlightPhrase highlight(
            currentUsername, settings.showSelfHighlightInMentions,
            settings.enableSelfHighlightTaskbar,
            settings.enableSelfHighlightSound, false, false,
            settings.selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight),
            std::make_shared<QColor>(selfMatchColor), selfMatchStyle);

        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    auto kickUser = getApp()->getAccounts()->kick.current();
    auto kickUsername = kickUser->username();
    if (settings.enableSelfHighlight && !kickUsername.isEmpty() &&
        !kickUser->isAnonymous())
    {
        HighlightPhrase highlight(
            kickUsername, settings.showSelfHighlightInMentions,
            settings.enableSelfHighlightTaskbar,
            settings.enableSelfHighlightSound, false, false,
            settings.selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight),
            std::make_shared<QColor>(selfMatchColor), selfMatchStyle);

        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    auto messageHighlights = settings.highlightedMessages.readOnly();
    for (const auto &highlight : *messageHighlights)
    {
        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    auto wordLists = settings.highlightWordLists.readOnly();
    for (const auto &wordList : *wordLists)
    {
        const bool hasMarker =
            wordList.matchStyle() != HighlightMatchStyle::None;
        checks.emplace_back(HighlightCheck{
            [wordList, hasMarker](
                const auto &, const auto &, const auto &,
                const auto &originalMessage, const auto &, const auto self,
                const auto platform, const auto &channelName,
                const auto collectMatches) -> std::optional<HighlightResult> {
                if (self)
                {
                    return std::nullopt;
                }
                // looking for where it matches costs more than whether
                auto match = wordList.matchForNormalizedChannel(
                    originalMessage, channelName, platform,
                    collectMatches && hasMarker
                        ? HighlightWordListMatchMode::WithRanges
                        : HighlightWordListMatchMode::MatchOnly);
                if (!match.matched)
                {
                    return std::nullopt;
                }

                std::optional<QUrl> soundUrl;
                if (wordList.hasCustomSound())
                {
                    soundUrl = wordList.soundUrl();
                }
                auto result = HighlightResult{
                    wordList.hasAlert(), wordList.hasSound(), soundUrl,
                    wordList.color(), wordList.showInMentions()};
                result.matches = std::move(match.matches);
                return result;
            }});
    }

    if (settings.enableAutomodHighlight)
    {
        const auto highlightSound =
            settings.enableAutomodHighlightSound.getValue();
        const auto highlightAlert =
            settings.enableAutomodHighlightTaskbar.getValue();
        const auto highlightSoundUrlValue =
            settings.automodHighlightSoundUrl.getValue();
        auto highlightColor =
            ColorProvider::instance().color(ColorType::AutomodHighlight);

        checks.emplace_back(HighlightCheck{
            [=](const auto &, const auto &, const auto &, const auto &,
                const auto &flags, const auto, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                if (!flags.has(MessageFlag::AutoModOffendingMessage))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (!highlightSoundUrlValue.isEmpty())
                {
                    highlightSoundUrl = highlightSoundUrlValue;
                }

                return HighlightResult{
                    highlightAlert, highlightSound, highlightSoundUrl,
                    highlightColor, false,
                };
            }});
    }
}

void rebuildUserHighlights(Settings &settings,
                           std::vector<HighlightCheck> &checks)
{
    auto userHighlights = settings.highlightedUsers.readOnly();

    if (settings.enableSelfMessageHighlight)
    {
        bool showInMentions = settings.showSelfMessageHighlightInMentions;

        checks.emplace_back(HighlightCheck{
            [showInMentions](
                const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto /*platform*/,
                const auto & /*channelName*/, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)senderName;
                (void)flags;
                (void)originalMessage;

                if (!self)
                {
                    return std::nullopt;
                }

                auto highlightColor = ColorProvider::instance().color(
                    ColorType::SelfMessageHighlight);

                return HighlightResult{false, false, (QUrl) nullptr,
                                       highlightColor, showInMentions};
            }});
    }

    for (const auto &highlight : *userHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags, const auto self, const auto platform,
                        const auto &channelName, const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!highlight.getChannelScope().appliesToNormalized(
                        platform, channelName) ||
                    !highlight.isMatch(senderName))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (highlight.hasCustomSound())
                {
                    highlightSoundUrl = highlight.getSoundUrl();
                }

                return HighlightResult{
                    highlight.hasAlert(),       highlight.hasSound(),
                    highlightSoundUrl,          highlight.getColor(),
                    highlight.showInMentions(),
                };
            }});
    }
}

void rebuildBadgeHighlights(Settings &settings,
                            std::vector<HighlightCheck> &checks)
{
    auto badgeHighlights = settings.highlightedBadges.readOnly();

    for (const auto &highlight : *badgeHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags, const auto self,
                        const auto /*platform*/, const auto & /*channelName*/,
                        const auto /*collectMatches*/)
                -> std::optional<HighlightResult> {
                (void)args;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                for (const TwitchBadge &badge : twitchBadges)
                {
                    if (highlight.isMatch(badge))
                    {
                        std::optional<QUrl> highlightSoundUrl;
                        if (highlight.hasCustomSound())
                        {
                            highlightSoundUrl = highlight.getSoundUrl();
                        }

                        return HighlightResult{
                            highlight.hasAlert(),       highlight.hasSound(),
                            highlightSoundUrl,          highlight.getColor(),
                            highlight.showInMentions(),
                        };
                    }
                }

                return std::nullopt;
            }});
    }
}

}  // namespace

namespace chatterino {

HighlightController::HighlightController(Settings &settings,
                                         AccountController *accounts)
{
    assert(accounts != nullptr);

    this->rebuildListener_.addSetting(settings.enableSelfHighlight);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.selfHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.selfHighlightMatchColor);
    this->rebuildListener_.addSetting(settings.selfHighlightMatchStyle);
    this->rebuildListener_.addSetting(settings.showSelfHighlightInMentions);

    this->rebuildListener_.addSetting(settings.enableWhisperHighlight);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightSound);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.whisperHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableSubHighlight);
    this->rebuildListener_.addSetting(settings.enableSubHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSubHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.enableSelfMessageHighlight);
    this->rebuildListener_.addSetting(
        settings.showSelfMessageHighlightInMentions);

    this->rebuildListener_.addSetting(settings.subHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableFollowHighlight);
    this->rebuildListener_.addSetting(settings.enableFollowHighlightSound);
    this->rebuildListener_.addSetting(settings.enableFollowHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.followHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableThreadHighlight);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightSound);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.threadHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.showThreadHighlightInMentions);

    this->rebuildListener_.addSetting(settings.enableAutomodHighlight);
    this->rebuildListener_.addSetting(settings.showAutomodInMentions);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightSound);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.automodHighlightSoundUrl);

    this->rebuildListener_.setCB([this, &settings] {
        qCDebug(chatterinoHighlights)
            << "Rebuild checks because a setting changed";
        this->rebuildChecks(settings);
    });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedBadges.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight badges changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedUsers.delayedItemsChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight users changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightWordLists.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight word lists changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedMessages.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight messages changed";
            this->rebuildChecks(settings);
        });

    this->bConnections.emplace_back(
        accounts->twitch.currentUserChanged.connect([this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user swapped accounts";
            this->rebuildChecks(settings);
        }));

    this->signalHolder_.managedConnect(
        accounts->twitch.currentUserNameChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user name changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->kick.currentUserChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because Kick user changed";
            this->rebuildChecks(settings);
        });

    this->rebuildChecks(settings);
}

void HighlightController::rebuildChecks(Settings &settings)
{
    auto checks = this->checks_.access();
    checks->clear();

    rebuildSubscriptionHighlights(settings, *checks);

    rebuildFollowHighlights(settings, *checks);

    rebuildWhisperHighlights(settings, *checks);

    rebuildMessageHighlights(settings, *checks);

    rebuildUserHighlights(settings, *checks);

    rebuildReplyThreadHighlight(settings, *checks);

    rebuildBadgeHighlights(settings, *checks);
}

std::pair<bool, HighlightResult> HighlightController::check(
    const MessageParseArgs &args, const std::vector<TwitchBadge> &twitchBadges,
    const QString &senderName, const QString &originalMessage,
    const MessageFlags &messageFlags, MessagePlatform platform,
    const QString &channelName, bool forceMatchRanges) const
{
    const auto normalizedChannelName =
        normalizeHighlightChannelName(channelName);
    const bool collectMatches =
        forceMatchRanges || getSettings()->highlightMatchedFragments;

    constexpr size_t MAX_MATCHES_PER_MESSAGE = 256;

    bool highlighted = false;
    auto result = HighlightResult::emptyResult();

    const auto checks = this->checks_.accessConst();

    bool self = false;
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch: {
            auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
            self = senderName == currentUser->getUserName();
        }
        break;
        case MessagePlatform::Kick: {
            auto kickUser = getApp()->getAccounts()->kick.current();
            self =
                !kickUser->isAnonymous() && senderName == kickUser->username();
        }
        break;
        case MessagePlatform::YouTube: {
        }
        break;
    }

    for (const auto &check : *checks)
    {
        if (auto checkResult = check.cb(
                args, twitchBadges, senderName, originalMessage, messageFlags,
                self, platform, normalizedChannelName, collectMatches);
            checkResult)
        {
            highlighted = true;

            if (checkResult->alert)
            {
                if (!result.alert)
                {
                    result.alert = checkResult->alert;
                }
            }

            if (checkResult->playSound)
            {
                if (!result.playSound)
                {
                    result.playSound = checkResult->playSound;
                }
            }

            if (checkResult->customSoundUrl)
            {
                if (!result.customSoundUrl)
                {
                    result.customSoundUrl = checkResult->customSoundUrl;
                }
            }

            if (checkResult->color)
            {
                if (!result.color)
                {
                    result.color = checkResult->color;
                }
                // Colors can change later, so same colors are only skipped
                // when painting.
                else if (result.extraColors.size() <
                             HighlightResult::MAX_EXTRA_COLORS &&
                         checkResult->color != result.color &&
                         std::ranges::find(result.extraColors,
                                           checkResult->color) ==
                             result.extraColors.end())
                {
                    result.extraColors.push_back(checkResult->color);
                }
            }

            if (!checkResult->matches.empty() &&
                result.matches.size() < MAX_MATCHES_PER_MESSAGE)
            {
                const auto count =
                    std::min(MAX_MATCHES_PER_MESSAGE - result.matches.size(),
                             checkResult->matches.size());
                result.matches.insert(result.matches.end(),
                                      checkResult->matches.begin(),
                                      checkResult->matches.begin() +
                                          static_cast<std::ptrdiff_t>(count));
            }

            if (checkResult->showInMentions)
            {
                if (!result.showInMentions)
                {
                    result.showInMentions = checkResult->showInMentions;
                }
            }

            // with markers, every phrase that matches is of interest
            if (result.full() &&
                result.extraColors.size() >=
                    HighlightResult::MAX_EXTRA_COLORS &&
                !collectMatches)
            {
                break;
            }
        }
    }

    return {highlighted, result};
}

}  // namespace chatterino
