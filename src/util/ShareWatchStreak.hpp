// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <memory>

class QWidget;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

/// Shares the Twitch watch streak of the logged in user in `channel`, with an
/// optional message, like the "Share" button of the streak on Twitch.
void shareWatchStreakIn(const ChannelPtr &channel, const QString &message);

/// The same, after asking for the optional message in a small box
void askAndShareWatchStreak(QWidget *parent, const ChannelPtr &channel);

}  // namespace chatterino
