// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/OwnBadgeLayout.hpp"

#include "messages/Emote.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Settings.hpp"

#include <QChar>
#include <QHash>
#include <QStringList>

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

namespace chatterino::ownbadges {

namespace {

/// Between the key and the label of a saved entry
constexpr QChar SEPARATOR = u'\t';

struct Provider {
    MessageElementFlag flag;
    QStringView key;
    QStringView name;
};

/// Twitch's own badges have a place in the order as a group each: the role
/// (broadcaster, moderator, VIP, staff), the subscription, the badge picked
/// globally or for the channel, and the prediction. They come first.
constexpr std::array<QStringView, 4> TWITCH_SLOTS{u"ta", u"ts", u"tv", u"tp"};

QStringView twitchSlot(MessageElementFlags flags)
{
    if (flags.hasAny(MessageElementFlag::BadgeGlobalAuthority,
                     MessageElementFlag::BadgeChannelAuthority))
    {
        return u"ta";
    }
    if (flags.has(MessageElementFlag::BadgeSubscription))
    {
        return u"ts";
    }
    if (flags.has(MessageElementFlag::BadgePredictions))
    {
        return u"tp";
    }
    return u"tv";
}

/// Where badges come from, in the order they are usually shown in, after
/// Twitch's own
constexpr std::array PROVIDERS{
    Provider{.flag = MessageElementFlag::BadgeChatterino,
             .key = u"c",
             .name = u"Chatterino"},
    Provider{.flag = MessageElementFlag::BadgeFfz,
             .key = u"ff",
             .name = u"FrankerFaceZ"},
    Provider{.flag = MessageElementFlag::BadgeFfzAp,
             .key = u"fa",
             .name = u"FFZ:AP"},
    Provider{.flag = MessageElementFlag::BadgeBttv,
             .key = u"bt",
             .name = u"BetterTTV"},
    Provider{.flag = MessageElementFlag::BadgeMoltorino,
             .key = u"m",
             .name = u"Moltorino"},
    Provider{.flag = MessageElementFlag::BadgeBluzyrino,
             .key = u"bl",
             .name = u"Bluzyrino"},
    Provider{
        .flag = MessageElementFlag::BadgeSevenTV, .key = u"7", .name = u"7TV"},
    Provider{.flag = MessageElementFlag::BadgeDankChat,
             .key = u"dc",
             .name = u"DankChat"},
    Provider{.flag = MessageElementFlag::BadgeChatsen,
             .key = u"cs",
             .name = u"Chatsen"},
    Provider{.flag = MessageElementFlag::BadgeHomiesCustom,
             .key = u"hc",
             .name = u"Homies"},
    Provider{.flag = MessageElementFlag::BadgeHomiesSupporter,
             .key = u"hs",
             .name = u"Homies"},
    Provider{.flag = MessageElementFlag::BadgeHomies,
             .key = u"h",
             .name = u"Homies"},
    Provider{.flag = MessageElementFlag::BadgeFolhinha,
             .key = u"fo",
             .name = u"FolhinhaBot"},
    Provider{.flag = MessageElementFlag::BadgeBadgeHQ,
             .key = u"bh",
             .name = u"BadgeHQ"},
    Provider{.flag = MessageElementFlag::BadgeLimerino,
             .key = u"li",
             .name = u"Limerino"},
    Provider{.flag = MessageElementFlag::BadgeJilChat,
             .key = u"jc",
             .name = u"JilChat"},
    Provider{
        .flag = MessageElementFlag::BadgeJiltv, .key = u"jt", .name = u"JilTV"},
};

/// The key of a badge and what to call it. The key is empty for
/// elements that have no place of their own in the order.
std::pair<QString, QString> describe(const MessageElement &element)
{
    const auto flags = element.getFlags();
    if (flags.has(MessageElementFlag::BadgeSharedChannel))
    {
        return {};
    }

    const auto *badge = dynamic_cast<const BadgeElement *>(&element);
    if (badge == nullptr)
    {
        return {};
    }

    const auto emote = badge->getEmote();
    const auto title = emote ? emote->tooltip.string : QString{};

    if (const auto slug = badge->twitchBadgeSlug())
    {
        return {
            twitchSlot(element.getFlags()).toString(),
            QStringLiteral("Twitch: ") + (title.isEmpty() ? *slug : title),
        };
    }

    for (const auto &provider : PROVIDERS)
    {
        if (flags.has(provider.flag))
        {
            const auto name = provider.name.toString();
            return {
                provider.key.toString(),
                title.isEmpty() ? name : name + QStringLiteral(": ") + title,
            };
        }
    }

    return {};
}

/// Where a badge goes if the user gave it no place: Twitch's groups
/// first, then the others like in PROVIDERS
int defaultRank(QStringView key)
{
    const auto twitch = std::ranges::distance(
        TWITCH_SLOTS.begin(), std::ranges::find(TWITCH_SLOTS, key));
    if (twitch < std::ssize(TWITCH_SLOTS))
    {
        return static_cast<int>(twitch);
    }
    const auto index = std::ranges::distance(
        PROVIDERS.begin(), std::ranges::find(PROVIDERS, key, &Provider::key));
    return static_cast<int>(std::ssize(TWITCH_SLOTS) + index);
}

std::mutex SEEN_MUTEX;

/// The badges seen on the user's messages: key and label, in the order
/// they were first seen in
std::vector<std::pair<QString, QString>> &seenBadges()
{
    static std::vector<std::pair<QString, QString>> seen;
    return seen;
}

/// Per channel: the groups of Twitch's badges the user's latest message
/// there had, and what the badge of each was called. Guarded by SEEN_MUTEX.
QHash<QString, QHash<QString, QString>> &twitchSlotsByChannel()
{
    static QHash<QString, QHash<QString, QString>> slots;
    return slots;
}

bool isTwitchSlot(QStringView key)
{
    return std::ranges::find(TWITCH_SLOTS, key) != TWITCH_SLOTS.end();
}

void remember(const QString &key, const QString &label)
{
    std::scoped_lock lock(SEEN_MUTEX);
    auto &seen = seenBadges();
    const auto it =
        std::ranges::find(seen, key, &std::pair<QString, QString>::first);
    if (it == seen.end())
    {
        seen.emplace_back(key, label);
    }
    else
    {
        // e.g. a subscriber badge of more months
        it->second = label;
    }
}

/// The saved order: keys with the label they had when they were saved
std::vector<std::pair<QString, QString>> savedOrder()
{
    std::vector<std::pair<QString, QString>> order;
    for (const auto &saved : getSettings()->ownBadgeOrder.getValue())
    {
        const auto separator = saved.indexOf(SEPARATOR);
        auto key = separator < 0 ? saved : saved.left(separator);
        // Twitch's own badges once had a place each; now they have one
        // per group
        if (key.isEmpty() || key.startsWith(u"t:"))
        {
            continue;
        }
        order.emplace_back(std::move(key), separator < 0
                                               ? QString{}
                                               : saved.mid(separator + 1));
    }
    return order;
}

}  // namespace

void apply(std::vector<std::unique_ptr<MessageElement>> &elements,
           size_t firstBadge, const QString &channelName)
{
    if (firstBadge >= elements.size())
    {
        return;
    }
    const auto begin = elements.begin() + static_cast<ptrdiff_t>(firstBadge);

    QHash<QString, QString> channelSlots;
    for (auto it = begin; it != elements.end(); ++it)
    {
        const auto [key, label] = describe(**it);
        if (!key.isEmpty())
        {
            remember(key, label);
            if (isTwitchSlot(key) && !channelSlots.contains(key))
            {
                channelSlots.insert(key, label);
            }
        }
    }
    if (!channelName.isEmpty())
    {
        // what the user is in that channel now, e.g. no longer a VIP
        std::scoped_lock lock(SEEN_MUTEX);
        twitchSlotsByChannel().insert(channelName.toLower(), channelSlots);
    }

    const auto hidden = getSettings()->ownBadgeHidden.getValue();
    const auto order = savedOrder();
    if (hidden.empty() && order.empty())
    {
        return;
    }

    elements.erase(std::remove_if(begin, elements.end(),
                                  [&](const auto &element) {
                                      const auto key = describe(*element).first;
                                      return !key.isEmpty() &&
                                             std::ranges::find(hidden, key) !=
                                                 hidden.end();
                                  }),
                   elements.end());

    QHash<QString, int> ranks;
    for (size_t index = 0; index < order.size(); ++index)
    {
        ranks.insert(order[index].first, static_cast<int>(index));
    }
    const auto rankOf = [&](const MessageElement &element) {
        const auto key = describe(element).first;
        if (key.isEmpty())
        {
            // stays in front, like the badge of a shared chat
            return -1;
        }
        return ranks.value(key,
                           static_cast<int>(order.size()) + defaultRank(key));
    };

    // stable: badges with the same rank keep the order they came in
    std::stable_sort(elements.begin() + static_cast<ptrdiff_t>(firstBadge),
                     elements.end(), [&](const auto &left, const auto &right) {
                         return rankOf(*left) < rankOf(*right);
                     });
}

void noteChannelBadges(const QString &channelName,
                       const std::vector<TwitchBadgeHeld> &badges)
{
    if (channelName.isEmpty())
    {
        return;
    }
    QHash<QString, QString> channelSlots;
    for (const auto &badge : badges)
    {
        const auto key = twitchSlot(MessageElementFlags{badge.flag}).toString();
        if (!channelSlots.contains(key))
        {
            const auto label = QStringLiteral("Twitch: ") + badge.title;
            channelSlots.insert(key, label);
            remember(key, label);
        }
    }
    std::scoped_lock lock(SEEN_MUTEX);
    twitchSlotsByChannel().insert(channelName.toLower(), channelSlots);
}

std::vector<Entry> entries(const QString &channelName)
{
    const auto hidden = getSettings()->ownBadgeHidden.getValue();
    const auto isVisible = [&](const QString &key) {
        return std::ranges::find(hidden, key) == hidden.end();
    };

    std::vector<std::pair<QString, QString>> seen;
    QHash<QString, QString> channelSlots;
    {
        std::scoped_lock lock(SEEN_MUTEX);
        seen = seenBadges();
        if (!channelName.isEmpty())
        {
            channelSlots = twitchSlotsByChannel().value(channelName.toLower());
        }
    }
    // Twitch's badges as they are in that channel, the others everywhere
    const auto fitChannel = [&](Entry &entry) {
        if (channelName.isEmpty() || !isTwitchSlot(entry.key))
        {
            return;
        }
        const auto found = channelSlots.constFind(entry.key);
        entry.available = found != channelSlots.cend();
        if (entry.available && !found->isEmpty())
        {
            entry.label = *found;
        }
    };

    const auto saved = savedOrder();
    std::vector<Entry> result;
    result.reserve(saved.size() + seen.size());
    for (const auto &[key, savedLabel] : saved)
    {
        // what the badge is called now, if it was seen since
        const auto it =
            std::ranges::find(seen, key, &std::pair<QString, QString>::first);
        auto label = it != seen.end() ? it->second : savedLabel;
        result.push_back({
            .key = key,
            .label = label.isEmpty() ? key : label,
            .visible = isVisible(key),
        });
        fitChannel(result.back());
    }

    // the ones without a place yet, where they would be shown
    std::ranges::stable_sort(seen, [](const auto &left, const auto &right) {
        return defaultRank(left.first) < defaultRank(right.first);
    });
    for (const auto &[key, label] : seen)
    {
        if (std::ranges::find(result, key, &Entry::key) == result.end())
        {
            result.push_back({
                .key = key,
                .label = label.isEmpty() ? key : label,
                .visible = isVisible(key),
            });
            fitChannel(result.back());
        }
    }
    return result;
}

void save(const std::vector<Entry> &entries)
{
    std::vector<QString> order;
    std::vector<QString> hidden;
    order.reserve(entries.size());
    for (const auto &entry : entries)
    {
        if (entry.key.isEmpty())
        {
            continue;
        }
        order.push_back(entry.key + SEPARATOR + entry.label);
        if (!entry.visible)
        {
            hidden.push_back(entry.key);
        }
    }
    getSettings()->ownBadgeOrder.setValue(order);
    getSettings()->ownBadgeHidden.setValue(hidden);
}

void reset()
{
    getSettings()->ownBadgeOrder.setValue(std::vector<QString>{});
    getSettings()->ownBadgeHidden.setValue(std::vector<QString>{});
}

bool isCustomized()
{
    return !getSettings()->ownBadgeOrder.getValue().empty() ||
           !getSettings()->ownBadgeHidden.getValue().empty();
}

}  // namespace chatterino::ownbadges
