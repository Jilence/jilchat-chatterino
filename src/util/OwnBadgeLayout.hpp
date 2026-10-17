// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace chatterino {

class MessageElement;
enum class MessageElementFlag : int64_t;

/// The order the user wants their own badges in, and which of them they hide.
/// This only changes how they see their own messages: it is kept on this
/// computer and nobody else gets to know it.
namespace ownbadges {

struct Entry {
    /// What kind of badge this is, e.g. "ta" for the Twitch role badge or
    /// "7" for 7TV
    QString key;
    /// What to call it, e.g. "Twitch: Broadcaster"
    QString label;
    bool visible = true;
    /// Whether the user has this badge in the channel the list is for.
    /// The others keep their place but aren't listed.
    bool available = true;
};

/// Puts the badges of one of the user's own messages in the order they
/// chose and takes out the ones they hide. Also remembers which badges
/// there are, for entries().
///
/// @param firstBadge Where the badges start in `elements`. Everything
///                   from there to the end has to be a badge.
/// @param channelName Where the message is from, to know which of Twitch's
///                    badges the user has there
void apply(std::vector<std::unique_ptr<MessageElement>> &elements,
           size_t firstBadge, const QString &channelName = {});

/// One of Twitch's badges the user has in a channel
struct TwitchBadgeHeld {
    MessageElementFlag flag;
    /// e.g. "Broadcaster"
    QString title;
};

/// What Twitch says the user has in a channel when joining it or after
/// sending a message there (USERSTATE), so its badges can be listed before
/// one of the user's messages was seen there
void noteChannelBadges(const QString &channelName,
                       const std::vector<TwitchBadgeHeld> &badges);

/// The badges that were seen on the user's messages or have a place in
/// the order, in the order they are shown in
///
/// @param channelName The channel to name Twitch's badges for: a group of
///                    them the user has no badge of there isn't available.
///                    Empty: every group, named as last seen anywhere.
std::vector<Entry> entries(const QString &channelName = {});

/// Makes `entries` the order, hiding those that aren't visible
void save(const std::vector<Entry> &entries);

/// Back to the usual order, with every badge shown
void reset();

/// Whether the user changed anything
bool isCustomized();

}  // namespace ownbadges

}  // namespace chatterino
