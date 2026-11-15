// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QRegion>

namespace chatterino {

/// Animated username paints get a new picture this many times a second (all
/// in lockstep). The chat repaints its animated parts 50 times a second for
/// GIFs; names with a paint only need it when this tick moves on.
inline constexpr qint64 ANIMATED_PAINT_FPS = 30;

/// Counts up ANIMATED_PAINT_FPS times a second.
inline qint64 animatedPaintTick()
{
    return QDateTime::currentMSecsSinceEpoch() * ANIMATED_PAINT_FPS / 1000;
}

/// While a chat draws its messages, the rectangles of names with an animated
/// paint are added here (nullptr otherwise), so the chat can repaint them at
/// the paints' own rate instead of the GIFs'.
inline QRegion *&animatedPaintRegionSink()
{
    static QRegion *sink = nullptr;
    return sink;
}

}  // namespace chatterino
