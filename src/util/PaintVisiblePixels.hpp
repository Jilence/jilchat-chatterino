// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHash>
#include <QRect>

namespace chatterino {

/// For pixmaps of animated username paints that are mostly transparent (a
/// wide room for the glow around the name): the part that has visible
/// pixels, in the pixmap's own pixels, by QPixmap::cacheKey(). The chat then
/// draws only that part instead of blending the whole transparent room on
/// every repaint. A paint adds its pixmaps and removes them again when it
/// drops them.
inline QHash<qint64, QRect> &paintVisiblePixels()
{
    static QHash<qint64, QRect> visible;
    return visible;
}

}  // namespace chatterino
