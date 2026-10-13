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

/// Where badges come from, in the order they are usually shown in.
/// Twitch's own come first and are told apart by their name.
constexpr std::array PROVIDERS{
    Provider{MessageElementFlag::BadgeChatterino, u"c", u"Chatterino"},
    Provider{MessageElementFlag::BadgeFfz, u"ff", u"FrankerFaceZ"},
    Provider{MessageElementFlag::BadgeFfzAp, u"fa", u"FFZ:AP"},
    Provider{MessageElementFlag::BadgeBttv, u"bt", u"BetterTTV"},
    Provider{MessageElementFlag::BadgeMoltorino, u"m", u"Moltorino"},
    Provider{MessageElementFlag::BadgeBluzyrino, u"bl", u"Bluzyrino"},
    Provider{MessageElementFlag::BadgeSevenTV, u"7", u"7TV"},
    Provider{MessageElementFlag::BadgeDankChat, u"dc", u"DankChat"},
    Provider{MessageElementFlag::BadgeChatsen, u"cs", u"Chatsen"},
    Provider{MessageElementFlag::BadgeHomiesCustom, u"hc", u"Homies"},
    Provider{MessageElementFlag::BadgeHomiesSupporter, u"hs", u"Homies"},
    Provider{MessageElementFlag::BadgeHomies, u"h", u"Homies"},
    Provider{MessageElementFlag::BadgeFolhinha, u"fo", u"FolhinhaBot"},
    Provider{MessageElementFlag::BadgeJilChat, u"jc", u"JilChat"},
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
            QStringLiteral("t:") + slug->trimmed().toLower(),
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

/// Where a badge goes if the user gave it no place: Twitch's first,
/// then the others like in PROVIDERS
int defaultRank(QStringView key)
{
    if (key.startsWith(u"t:"))
    {
        return 0;
    }
    const auto it = std::ranges::find(PROVIDERS, key, &Provider::key);
    return 1 + static_cast<int>(it - PROVIDERS.begin());
}

std::mutex SEEN_MUTEX;

/// The badges seen on the user's messages: key and label, in the order
/// they were first seen in
std::vector<std::pair<QString, QString>> &seenBadges()
{
    static std::vector<std::pair<QString, QString>> seen;
    return seen;
}

void remember(const QString &key, const QString &label)
{
    std::lock_guard lock(SEEN_MUTEX);
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
        if (key.isEmpty())
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
           size_t firstBadge)
{
    if (firstBadge >= elements.size())
    {
        return;
    }
    const auto begin = elements.begin() + static_cast<ptrdiff_t>(firstBadge);

    for (auto it = begin; it != elements.end(); ++it)
    {
        const auto [key, label] = describe(**it);
        if (!key.isEmpty())
        {
            remember(key, label);
        }
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
    for (int index = 0; index < static_cast<int>(order.size()); ++index)
    {
        ranks.insert(order[static_cast<size_t>(index)].first, index);
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

std::vector<Entry> entries()
{
    const auto hidden = getSettings()->ownBadgeHidden.getValue();
    const auto isVisible = [&](const QString &key) {
        return std::ranges::find(hidden, key) == hidden.end();
    };

    std::vector<std::pair<QString, QString>> seen;
    {
        std::lock_guard lock(SEEN_MUTEX);
        seen = seenBadges();
    }

    std::vector<Entry> result;
    for (const auto &[key, savedLabel] : savedOrder())
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
    }

    // the ones without a place yet, where they would be shown
    std::stable_sort(
        seen.begin(), seen.end(), [](const auto &left, const auto &right) {
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
