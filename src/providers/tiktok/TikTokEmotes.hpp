// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"

namespace chatterino {
std::shared_ptr<const EmoteMap> tikTokBuiltinEmotes();
EmotePtr tikTokEmote(QStringView name, QStringView url, int height = 28);
}  // namespace chatterino
