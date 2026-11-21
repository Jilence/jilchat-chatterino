// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QRectF>
#include <QString>

class QPainter;

namespace chatterino::jilchatcard {

/// The black and white look of the setup, shared by the windows the app shows
/// in the same style (the update and the portable welcome)

inline const QColor TEXT(244, 244, 246);
inline const QColor MUTED(150, 150, 156);
inline const QColor INK(9, 9, 10);

/// How far the soft shadow reaches around a card
constexpr int SHADOW = 24;
constexpr double RADIUS = 16;

QFont font(int pixels, bool strong);

/// The white logo
const QPixmap &logo();

/// The card itself: shadow, background, the glow around `glowCenter` and the
/// rising sparks. `seconds` drives the animations.
void drawCard(QPainter &painter, const QRectF &card, double seconds,
              QPointF glowCenter, double glowRadius);

/// The logo with a darker band sweeping over it
void drawLogo(QPainter &painter, const QRectF &area, double seconds,
              double opacity = 1);

/// A white button (primary) or a quiet outlined one. With `busy`, a light
/// passes along it.
void drawButton(QPainter &painter, const QRectF &area, const QString &text,
                bool primary, double hover, bool busy, double seconds);

void drawClose(QPainter &painter, const QRectF &area, double hover);

/// A small rounded label, like "NEW UPDATE"
void drawPill(QPainter &painter, const QRectF &area, const QString &text,
              const QColor &textColor, const QColor &fill,
              const QColor &border);

}  // namespace chatterino::jilchatcard
