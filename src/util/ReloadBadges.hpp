// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

/// Loads the badges of the third-party providers again (Homies, FolhinhaBot,
/// Moltorino, JilChat, Bluzyrino, DankChat, Chatterino, FrankerFaceZ, FFZ:AP,
/// Chatsen).
///
/// Only the providers whose badges are set to be shown are loaded. Each of
/// them tells `channel` once its answer is there, e.g. "Homies badges
/// reloaded." or "Failed to reload Homies badges.".
void reloadBadgeProviders(const ChannelPtr &channel);

}  // namespace chatterino
