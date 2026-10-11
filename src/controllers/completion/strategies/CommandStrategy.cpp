// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/strategies/CommandStrategy.hpp"

#include <algorithm>

namespace chatterino::completion {

QString commandPrefix(const CommandItem &item)
{
    return item.prefix.isEmpty() ? QStringLiteral("/") : item.prefix;
}

QString longestQueryPrefix(const std::vector<CommandItem> &items,
                           const QString &query)
{
    QString longest;
    for (const auto &item : items)
    {
        const auto prefix = commandPrefix(item);
        if (prefix.size() > longest.size() &&
            query.startsWith(prefix, Qt::CaseInsensitive))
        {
            longest = prefix;
        }
    }
    return longest;
}

QString normalizeQuery(const QString &query)
{
    if (query.startsWith('/') || query.startsWith('.') ||
        query.startsWith('#') || query.startsWith('$'))
    {
        return query.mid(1);
    }

    return query;
}

CommandStrategy::CommandStrategy(bool startsWithOnly)
    : startsWithOnly_(startsWithOnly)
{
}

void CommandStrategy::apply(const std::vector<CommandItem> &items,
                            std::vector<CommandItem> &output,
                            const QString &query) const
{
    auto alreadyAdded = [&](const CommandItem &item) {
        const auto prefix = commandPrefix(item);
        return std::any_of(
            output.begin(), output.end(), [&](const CommandItem &out) {
                return commandPrefix(out).compare(prefix,
                                                  Qt::CaseInsensitive) == 0 &&
                       out.name.compare(item.name, Qt::CaseInsensitive) == 0;
            });
    };
    auto addMatches = [&](const auto &predicate) {
        for (const auto &item : items)
        {
            if (!predicate(item) || alreadyAdded(item))
            {
                continue;
            }
            output.push_back(item);
        }
    };

    const auto longest = longestQueryPrefix(items, query);
    if (!longest.isEmpty())
    {
        const auto normalized = query.mid(longest.size());
        auto samePrefix = [&](const CommandItem &item) {
            return commandPrefix(item).compare(longest, Qt::CaseInsensitive) ==
                   0;
        };
        addMatches([&](const CommandItem &item) {
            return samePrefix(item) &&
                   item.name.compare(normalized, Qt::CaseInsensitive) == 0;
        });
        addMatches([&](const CommandItem &item) {
            return samePrefix(item) &&
                   item.name.startsWith(normalized, Qt::CaseInsensitive);
        });
        if (!this->startsWithOnly_)
        {
            addMatches([&](const CommandItem &item) {
                return samePrefix(item) &&
                       item.name.contains(normalized, Qt::CaseInsensitive);
            });
        }
        // A custom command stored as its whole trigger still matches while
        // that trigger is only partly typed. Named prefixes such as $$ stay
        // on the longest prefix selected above.
        addMatches([&](const CommandItem &item) {
            return item.name.isEmpty() && !item.prefix.isEmpty() &&
                   item.prefix.size() > query.size() &&
                   item.prefix.startsWith(query, Qt::CaseInsensitive);
        });
        return;
    }

    QString normalizedQuery = normalizeQuery(query);
    const auto preferredPrefix =
        (query.startsWith('/') || query.startsWith('.') ||
         query.startsWith('#') || query.startsWith('$'))
            ? query.at(0)
            : QChar{};
    const auto hasPreferredPrefix = !preferredPrefix.isNull();
    auto prefixFits = [&](const CommandItem &item) {
        return !hasPreferredPrefix ||
               item.prefix == QString(preferredPrefix) ||
               (preferredPrefix == QChar('/') && item.prefix.isEmpty());
    };
    auto accept = [&](const CommandItem &item, const auto &nameMatches) {
        if (!prefixFits(item))
        {
            return false;
        }
        if (item.name.isEmpty() && !item.prefix.isEmpty())
        {
            return item.prefix.startsWith(query, Qt::CaseInsensitive);
        }
        return nameMatches(item);
    };

    addMatches([&](const CommandItem &item) {
        return accept(item, [&](const CommandItem &candidate) {
            return candidate.name.compare(normalizedQuery,
                                          Qt::CaseInsensitive) == 0;
        });
    });
    addMatches([&](const CommandItem &item) {
        return accept(item, [&](const CommandItem &candidate) {
            return candidate.name.startsWith(normalizedQuery,
                                             Qt::CaseInsensitive);
        });
    });
    if (!this->startsWithOnly_)
    {
        addMatches([&](const CommandItem &item) {
            return accept(item, [&](const CommandItem &candidate) {
                return candidate.name.contains(normalizedQuery,
                                               Qt::CaseInsensitive);
            });
        });
    }
};

}  // namespace chatterino::completion
