// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QRect>
#include <QRegion>

#include <memory>
#include <vector>

namespace chatterino {

class Image;

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

/// Names whose paint is made of images (they change with the GIF timer)
/// report themselves here while a chat draws, so they are repainted whole
/// on every tick even where an animated image lies close by.
inline QRegion *&animatedImagePaintRegionSink()
{
    static QRegion *sink = nullptr;
    return sink;
}

/// An animated image (GIF emote or badge) as the chat drew it: where, and
/// which of its pictures it showed. The chat repaints the spot only once the
/// image shows another picture; most GIFs change far less often than the
/// chat's 50 repaints a second.
struct AnimatedImageSpot {
    QRect rect;
    std::shared_ptr<Image> image;
    /// QPixmap::cacheKey() of the picture that was drawn
    qint64 shown = 0;
};

/// While a chat draws its messages, animated images report themselves here
/// (nullptr otherwise).
inline std::vector<AnimatedImageSpot> *&animatedImageSpotSink()
{
    static std::vector<AnimatedImageSpot> *sink = nullptr;
    return sink;
}

}  // namespace chatterino
