// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <memory>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

/// When the setting is on, shouts out `raiderLogin` a few seconds after it
/// raided `channel`, if the user moderates it and no one else did already.
void shoutoutRaider(const ChannelPtr &channel, const QString &raiderLogin);

}  // namespace chatterino
