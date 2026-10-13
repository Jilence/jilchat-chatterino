// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

class MessageElement;

/// The order the user wants their own badges in, and which of them they hide.
/// This only changes how they see their own messages: it is kept on this
/// computer and nobody else gets to know it.
namespace ownbadges {

struct Entry {
    /// What kind of badge this is, e.g. "t:subscriber" or "7"
    QString key;
    /// What to call it, e.g. "Twitch: 1.5-Year Subscriber"
    QString label;
    bool visible = true;
};

/// Puts the badges of one of the user's own messages in the order they
/// chose and takes out the ones they hide. Also remembers which badges
/// there are, for entries().
///
/// @param firstBadge Where the badges start in `elements`. Everything
///                   from there to the end has to be a badge.
void apply(std::vector<std::unique_ptr<MessageElement>> &elements,
           size_t firstBadge);

/// The badges that were seen on the user's messages or have a place in
/// the order, in the order they are shown in
std::vector<Entry> entries();

/// Makes `entries` the order, hiding those that aren't visible
void save(const std::vector<Entry> &entries);

/// Back to the usual order, with every badge shown
void reset();

/// Whether the user changed anything
bool isCustomized();

}  // namespace ownbadges

}  // namespace chatterino
