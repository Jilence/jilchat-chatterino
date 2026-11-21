#pragma once

#include <QColor>
#include <QMarginsF>

class QPixmapDropShadowFilter;

namespace chatterino {

class PaintDropShadow
{
public:
    /// With `exactRadius`, the setting for larger 7TV shadows doesn't
    /// apply.
    PaintDropShadow(float xOffset, float yOffset, float radius, QColor color,
                    bool exactRadius = false);

    bool isValid() const;
    PaintDropShadow scaled(float scale) const;
    void apply(QPixmapDropShadowFilter &effect) const;

    /// Approximate space needed below the painted text for the shadow.
    float extentBelow() const;

    /// How far the shadow reaches past the text on each side, at `scale`
    QMarginsF margins(float scale) const;

private:
    const float xOffset_;
    const float yOffset_;
    const float radius_;
    const QColor color_;
    const bool exactRadius_;
};

}  // namespace chatterino
