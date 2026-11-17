// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>

#include <memory>

class QPainter;

namespace chatterino {

class Paint;

/// The paint of a channel's broadcaster, for its name on tabs and in the
/// channel bar (with the "Paints on channel names" setting). Whatever paint
/// the chat would draw on their name; for 7TV, their paint is also looked up
/// once by the channel's ID, since a broadcaster who hasn't chatted isn't
/// known yet. nullptr while unknown, without a paint, or with the setting
/// off.
std::shared_ptr<Paint> channelNamePaint(const QString &login,
                                        const QString &roomId);

class TwitchChannel;

/// The broadcaster's Twitch name color, for their name on tabs and in the
/// channel bar (with the "Paints on channel names" setting): from the chat
/// when they wrote in it, otherwise asked from Twitch once. Invalid while
/// unknown, without a color set, or with the setting off.
QColor channelNameColor(TwitchChannel &channel);

/// Fires when a channel's paint was looked up, so names can be drawn again.
pajlada::Signals::NoArgSignal &channelNamePaintsChanged();

/// Draws `text` in `rect` with `paint`, aligned like QPainter::drawText
/// would. `color` is the color below the paint (where it's transparent).
void drawPaintedName(QPainter &painter, const QRectF &rect, const QString &text,
                     const QFont &font, QColor color, const Paint &paint,
                     Qt::Alignment alignment, float scale);

}  // namespace chatterino
