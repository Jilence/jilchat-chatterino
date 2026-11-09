#pragma once

#include "providers/seventv/paints/PaintDropShadow.hpp"

#include <QBrush>
#include <QFont>
#include <QMarginsF>

#include <vector>

namespace chatterino {

class Paint
{
public:
    virtual QBrush asBrush(QColor userColor, QRectF drawingRect) const = 0;
    virtual const std::vector<PaintDropShadow> &getDropShadows() const = 0;
    virtual bool animated() const = 0;
    /// Whether the drop shadows are drawn; a setting of the paint's kind.
    virtual bool shadowsEnabled() const;
    /// How far the paint draws beyond the text on each side, for a paint
    /// that takes `padding` in getPixmap without centering as well. The
    /// chat then gives it that room around the name.
    /// 7TV paints leave room for their shadows, so these aren't cut off
    /// at the edge of the name.
    virtual qreal overflow(float scale) const;
    /// How much room the paint gets around the name on each side; what
    /// reaches further is cut off. For 7TV paints that's their shadows, at
    /// most 8 px to the sides and 4 px above and below (times `scale`).
    virtual QMarginsF overflowMargins(float scale) const;
    /// Whose paint this is, to tell the user: "7TV" unless said otherwise.
    virtual QString sourceName() const;

    /// With `centerVertically`, `padding` adds room on all sides of the
    /// text, so shadows and glows around it aren't cut off.
    virtual QPixmap getPixmap(const QString &text, const QFont &font,
                              QColor userColor, QSizeF size, float scale,
                              float dpr, bool centerVertically = false,
                              qreal padding = 0) const;

    /// The name in chat: `textSize` with `margins` of room around it, the
    /// whole pixmap that size. Text and paint are drawn inside the margins,
    /// shadows over all of it; what reaches past that is cut off.
    virtual QPixmap getNametagPixmap(const QString &text, const QFont &font,
                                     QColor userColor, QSizeF textSize,
                                     float scale, float dpr,
                                     QMarginsF margins) const;
    QString getNametagPixmapCacheKey(const QString &text, const QFont &font,
                                     QColor userColor, QSizeF textSize,
                                     float scale, float dpr,
                                     QMarginsF margins) const;

    /// Names what getPixmap draws for these arguments. Two calls with the
    /// same key give pixmaps of the same shape; used to keep things derived
    /// from the pixmap, like a shadow, instead of making them again.
    QString getPixmapCacheKey(const QString &text, const QFont &font,
                              QColor userColor, QSizeF size, float scale,
                              float dpr, bool centerVertically = false,
                              qreal padding = 0) const;

    Paint(QString id, QString name = {})
        : id(std::move(id))
        , name(std::move(name))
    {
    }
    virtual ~Paint() = default;

    Paint(const Paint &) = default;
    Paint(Paint &&) = delete;
    Paint &operator=(const Paint &) = default;
    Paint &operator=(Paint &&) = delete;

    QString id;
    QString name;

protected:
    /// getNametagPixmap for paints with their own getPixmap: drawn with
    /// overflow() on every side, then cut to `margins`
    QPixmap croppedNametagPixmap(const QString &text, const QFont &font,
                                 QColor userColor, QSizeF textSize, float scale,
                                 float dpr, QMarginsF margins) const;

    /// The identity of the image the paint is made of right now, for paints
    /// that are an (animated) image. 0 for all others.
    virtual qint64 sourcePixmapCacheKey() const;

    static QColor overlayColors(QColor background, QColor foreground);
    static qreal offsetRepeatingStopPosition(qreal position,
                                             const QGradientStops &stops);
};

}  // namespace chatterino
