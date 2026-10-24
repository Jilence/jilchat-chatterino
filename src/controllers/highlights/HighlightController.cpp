// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightChannelScope.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "controllers/highlights/Sounds.hpp"
#include "controllers/highlights/types/AnyHighlight.hpp"
#include "controllers/highlights/types/Common.hpp"
#include "controllers/highlights/types/LowTrustUserHighlight.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep
#include "singletons/Settings.hpp"

#include <algorithm>

namespace chatterino {

template <typename T>
concept SupportsValidityCheck = requires(T a) {
    { a.isValid() } -> std::same_as<bool>;
};

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoHighlights;

bool rebuildSharedHighlights(Settings &settings,
                             std::vector<HighlightCheck> &checks)
{
    bool isSubscribedThreadHighlightEnabled = false;

    auto highlights = settings.sharedHighlights.readOnly();

    for (const auto &highlight : *highlights)
    {
        auto enabled = highlights::isEnabled(highlight);

        if (!enabled)
        {
            continue;
        }

        isSubscribedThreadHighlightEnabled |=
            highlights::getID(highlight) ==
            highlights::SubscribedThreadHighlight::ID;

        std::visit(
            [&checks](auto &&h) {
                // buildCheck may return an empty function if its parameters were invalid (e.g. if a badge highlight has an invalid badge)
                auto check = h.buildCheck();
                if (check)
                {
                    checks.emplace_back(std::move(check));
                }
            },
            highlight);
    }

    return isSubscribedThreadHighlightEnabled;
}

void rebuildWordListHighlights(Settings &settings,
                               std::vector<HighlightCheck> &checks)
{
    auto wordLists = settings.highlightWordLists.readOnly();
    for (const auto &wordList : *wordLists)
    {
        if (!wordList.enabled())
        {
            continue;
        }

        QUrl sound;
        if (wordList.hasSound())
        {
            if (wordList.hasCustomSound())
            {
                sound = wordList.soundUrl();
            }
            else if (auto defaultSound =
                         highlights::resolveDefaultSound(u"001-ping2"))
            {
                sound = QUrl{defaultSound->resourcePath};
            }
        }
        const bool hasMarker =
            wordList.matchStyle() != HighlightMatchStyle::None;

        checks.emplace_back(
            [wordList, sound, hasMarker](const HighlightCheckParams &p)
                -> std::optional<HighlightResult> {
                if (p.self)
                {
                    return std::nullopt;
                }
                // looking for where it matches costs more than whether
                auto match = wordList.matchForNormalizedChannel(
                    p.originalMessage,
                    normalizeHighlightChannelName(p.channelName), p.platform,
                    hasMarker ? HighlightWordListMatchMode::WithRanges
                              : HighlightWordListMatchMode::MatchOnly);
                if (!match.matched)
                {
                    return std::nullopt;
                }

                auto result = HighlightResult{
                    .alert = wordList.hasAlert(),
                    .sound = sound,
                    .color = wordList.color(),
                    .showInMentions = wordList.showInMentions(),
                };
                result.matches = std::move(match.matches);
                return result;
            });
    }
}

}  // namespace

HighlightController::HighlightController(Settings &settings,
                                         AccountController *accounts)
{
    assert(accounts != nullptr);

    // currentUserChanged is boost::signals2 in leafyrino, not a pajlada signal.
    this->bConnections.emplace_back(
        accounts->twitch.currentUserChanged.connect(
            [this, &settings] {
                qCDebug(LOG)
                    << "Rebuild checks because Twitch user swapped accounts";
                this->rebuildChecks(settings);
            }));

    this->signalHolder_.managedConnect(
        accounts->twitch.currentUserNameChanged, [this, &settings] {
            qCDebug(LOG) << "Rebuild checks because Twitch user name changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->kick.currentUserChanged, [this, &settings] {
            qCDebug(LOG) << "Rebuild checks because Kick user swapped accounts";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->sharedHighlights.delayedItemsChanged, [this, &settings] {
            qCDebug(LOG) << "Rebuild checks because shared highlights changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightWordLists.delayedItemsChanged,
        [this, &settings] {
            qCDebug(LOG) << "Rebuild checks because word lists changed";
            this->rebuildChecks(settings);
        });

    this->rebuildChecks(settings);
}

void HighlightController::rebuildChecks(Settings &settings)
{
    // Access checks for modification
    auto checks = this->checks_.access();
    checks->clear();

    this->isSubscribedThreadHighlightEnabled =
        rebuildSharedHighlights(settings, *checks);
    rebuildWordListHighlights(settings, *checks);
}

std::pair<bool, HighlightResult> HighlightController::check(
    const HighlightCheckParams &params) const
{
    constexpr size_t MAX_MATCHES_PER_MESSAGE = 256;

    bool highlighted = false;
    auto result = HighlightResult::emptyResult();

    // Access for checking
    const auto checks = this->checks_.accessConst();

    for (const auto &check : *checks)
    {
        if (auto checkResult = check(params))
        {
            highlighted = true;

            if (checkResult->alert)
            {
                if (!result.alert)
                {
                    result.alert = checkResult->alert;
                }
            }

            if (checkResult->sound.isValid())
            {
                if (result.sound.isEmpty())
                {
                    result.sound = checkResult->sound;
                }
            }

            if (checkResult->color)
            {
                if (checkResult->color->isValid())
                {
                    if (!result.color)
                    {
                        result.color = checkResult->color;
                    }
                    else if (*result.color != *checkResult->color &&
                             std::ranges::none_of(
                                 result.extraColors, [&](const auto &c) {
                                     return *c == *checkResult->color;
                                 }))
                    {
                        result.extraColors.push_back(checkResult->color);
                    }
                }
            }

            if (checkResult->showInMentions)
            {
                if (!result.showInMentions)
                {
                    result.showInMentions = checkResult->showInMentions;
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

            // No early out once the result is full: every further match can
            // still add a color band
        }
    }

    return {highlighted, result};
}

std::unordered_set<QStringView> HighlightController::billTinHighlights()
{
    using namespace chatterino::highlights;

    return {
        YourUsernameHighlight::ID,               //
        WhispersHighlight::ID,                   //
        AnnouncementsHighlight::ID,              //
        SubscriptionsHighlight::ID,              //
        FollowHighlight::ID,                     //
        ChannelPointsHighlight::ID,              //
        FirstMessageHighlight::ID,               //
        SubscribedThreadHighlight::ID,           //
        AutomodCaughtHighlight::ID,              //
        WatchStreakHighlight::ID,                //
        YourMessagesHighlight::ID,               //
        LowTrustUserHighlight::ID,               //
        UncategorizedNotificationHighlight::ID,  //
    };
}

}  // namespace chatterino
