#include "providers/seventv/paints/Paint.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <private/qpixmapfilter_p.h>
#include <QFontMetricsF>
#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

qreal textBaseline(const QFont &font, const QRectF &rect)
{
    const QFontMetricsF metrics(font);

    return rect.bottom() - metrics.descent();
}

}  // namespace

bool Paint::shadowsEnabled() const
{
    return getSettings()->displaySevenTVPaintShadows;
}

qreal Paint::overflow(float scale) const
{
    const auto margins = this->overflowMargins(scale);
    return std::max(
        {margins.left(), margins.top(), margins.right(), margins.bottom()});
}

QMarginsF Paint::overflowMargins(float scale) const
{
    if (!this->shadowsEnabled())
    {
        return {};
    }
    QMarginsF shadow;
    for (const auto &dropShadow : this->getDropShadows())
    {
        shadow += dropShadow.margins(scale);
    }
    const auto horizontal = 8.0 * scale;
    const auto vertical = 4.0 * scale;
    return {
        std::clamp<qreal>(shadow.left(), 0, horizontal),
        std::clamp<qreal>(shadow.top(), 0, vertical),
        std::clamp<qreal>(shadow.right(), 0, horizontal),
        std::clamp<qreal>(shadow.bottom(), 0, vertical),
    };
}

QString Paint::sourceName() const
{
    return QStringLiteral("7TV");
}

qint64 Paint::sourcePixmapCacheKey() const
{
    return 0;
}

QString Paint::getPixmapCacheKey(const QString &text, const QFont &font,
                                 QColor userColor, QSizeF size, float scale,
                                 float dpr, bool centerVertically,
                                 qreal padding) const
{
    QString key;
    const auto fontKey = font.key();
    key.reserve(this->id.size() + text.size() + fontKey.size() + 96);
    // lengths first, so no part can run into the next
    const auto appendString = [&key](QStringView value) {
        key += QString::number(value.size());
        key += u':';
        key += value;
        key += u'/';
    };

    appendString(this->id);
    appendString(text);
    appendString(fontKey);
    key += QString::number(this->sourcePixmapCacheKey()) + u'/' +
           QString::number(userColor.rgba()) + u'/' +
           QString::number(qRound64(size.width() * dpr)) + u'x' +
           QString::number(qRound64(size.height() * dpr)) + u'/' +
           QString::number(qRound64(scale * 1000)) + u'/' +
           QString::number(qRound64(dpr * 1000)) + u'/' +
           QString::number(qRound64(std::max<qreal>(0, padding) * dpr)) + u'/' +
           (centerVertically ? u'1' : u'0') + u'/' +
           (this->shadowsEnabled() ? u'1' : u'0');
    return key;
}

QPixmap Paint::getPixmap(const QString &text, const QFont &font,
                         QColor userColor, QSizeF size, float scale, float dpr,
                         bool centerVertically, qreal padding) const
{
    QSizeF drawSize = size;
    if (centerVertically && this->shadowsEnabled())
    {
        float shadowExtent = 0;
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }

            shadowExtent = std::max(shadowExtent,
                                    shadow.scaled(scale / dpr).extentBelow());
        }

        drawSize.setHeight(size.height() + shadowExtent);
    }
    if (!centerVertically)
    {
        padding = 0;
    }
    drawSize += QSizeF(2 * padding, 2 * padding);

    QPixmap pixmap((drawSize * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter pixmapPainter(&pixmap);
    pixmapPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    pixmapPainter.setFont(font);

    const QRectF pixmapRect(QPointF{}, drawSize);
    const QRectF textRect(QPointF{padding, padding}, size);

    // NOTE: draw colon separately from the nametag
    // otherwise the paint would extend onto the colon
    bool drawColon = false;
    QRectF nametagBoundingRect = centerVertically ? textRect : pixmapRect;
    QString nametagText = text;
    if (nametagText.endsWith(':'))
    {
        drawColon = true;
        nametagText = nametagText.chopped(1);
        const auto textBounds = pixmapPainter.boundingRect(
            QRectF(0, 0, 10000, 10000), nametagText,
            QTextOption(Qt::AlignLeft | Qt::AlignTop));
        nametagBoundingRect =
            QRectF(0, 0, textBounds.width(),
                   centerVertically ? textRect.height() : pixmapRect.height());
    }

    QPen pen;
    const QBrush brush = this->asBrush(userColor, nametagBoundingRect);
    pen.setBrush(brush);
    pixmapPainter.setPen(pen);

    if (centerVertically)
    {
        pixmapPainter.drawText(nametagBoundingRect, nametagText,
                               QTextOption(Qt::AlignLeft | Qt::AlignVCenter));
    }
    else
    {
        const auto baseline = textBaseline(font, nametagBoundingRect);
        pixmapPainter.drawText(QPointF(nametagBoundingRect.left(), baseline),
                               nametagText);
    }
    pixmapPainter.end();

    if (!this->getDropShadows().empty() && this->shadowsEnabled())
    {
        QPixmap outMap((drawSize * dpr).toSize());
        outMap.setDevicePixelRatio(dpr);
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }
            outMap.fill(Qt::transparent);

            {
                QPainter outPainter(&outMap);
                auto scaled = shadow.scaled(
                    scale / static_cast<float>(outMap.devicePixelRatio()));

                QPixmapDropShadowFilter filter;
                scaled.apply(filter);
                filter.draw(&outPainter, {0, 0}, pixmap);
            }
            outMap.swap(pixmap);
        }
    }

    if (drawColon)
    {
        auto colonColor = getApp()->getThemes()->messages.textColors.regular;
        const auto baseline = textBaseline(font, nametagBoundingRect);

        pixmapPainter.begin(&pixmap);

        pixmapPainter.setPen(QPen(colonColor));
        pixmapPainter.setFont(font);

        pixmapPainter.drawText(QPointF(nametagBoundingRect.right(), baseline),
                               u":"_s);
        pixmapPainter.end();
    }

    return pixmap;
}

QPixmap Paint::getNametagPixmap(const QString &text, const QFont &font,
                                QColor userColor, QSizeF textSize, float scale,
                                float dpr, QMarginsF margins) const
{
    margins = {
        std::max<qreal>(0, margins.left()),
        std::max<qreal>(0, margins.top()),
        std::max<qreal>(0, margins.right()),
        std::max<qreal>(0, margins.bottom()),
    };
    const QSizeF size(textSize.width() + margins.left() + margins.right(),
                      textSize.height() + margins.top() + margins.bottom());
    if (textSize.width() <= 0 || textSize.height() <= 0)
    {
        margins = {};
        textSize = size;
    }

    QPixmap pixmap((size * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    const bool hasMargins = margins.left() > 0 || margins.top() > 0 ||
                            margins.right() > 0 || margins.bottom() > 0;
    QPixmap contentPixmap;
    auto *contentTarget = &pixmap;
    if (hasMargins)
    {
        contentPixmap = QPixmap((textSize * dpr).toSize());
        contentPixmap.setDevicePixelRatio(dpr);
        contentPixmap.fill(Qt::transparent);
        contentTarget = &contentPixmap;
    }

    QPainter contentPainter(contentTarget);
    contentPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    contentPainter.setFont(font);

    const QRectF contentRect(QPointF{}, textSize);

    // The colon is drawn on its own, so the paint doesn't run onto it
    bool drawColon = false;
    QRectF nametagBoundingRect = contentRect;
    QString nametagText = text;
    if (nametagText.endsWith(':'))
    {
        drawColon = true;
        nametagText = nametagText.chopped(1);
        const auto textBounds = contentPainter.boundingRect(
            QRectF(0, 0, 10000, 10000), nametagText,
            QTextOption(Qt::AlignLeft | Qt::AlignTop));
        nametagBoundingRect.setWidth(
            std::min(textBounds.width(), contentRect.width()));
    }

    QPen pen;
    pen.setBrush(this->asBrush(userColor, nametagBoundingRect));
    contentPainter.setPen(pen);

    const auto baseline = textBaseline(font, nametagBoundingRect);
    contentPainter.drawText(QPointF(nametagBoundingRect.left(), baseline),
                            nametagText);
    contentPainter.end();

    if (hasMargins)
    {
        QPainter pixmapPainter(&pixmap);
        pixmapPainter.drawPixmap(QPointF(margins.left(), margins.top()),
                                 contentPixmap);
    }

    if (!this->getDropShadows().empty() && this->shadowsEnabled())
    {
        QPixmap outMap((size * dpr).toSize());
        outMap.setDevicePixelRatio(dpr);
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }
            outMap.fill(Qt::transparent);

            {
                QPainter outPainter(&outMap);
                auto scaled = shadow.scaled(
                    scale / static_cast<float>(outMap.devicePixelRatio()));

                QPixmapDropShadowFilter filter;
                scaled.apply(filter);
                filter.draw(&outPainter, {0, 0}, pixmap);
            }
            outMap.swap(pixmap);
        }
    }

    if (drawColon)
    {
        auto colonColor = getApp()->getThemes()->messages.textColors.regular;

        QPainter pixmapPainter(&pixmap);
        pixmapPainter.setPen(QPen(colonColor));
        pixmapPainter.setFont(font);
        pixmapPainter.drawText(
            QPointF(margins.left() + nametagBoundingRect.right(),
                    margins.top() + baseline),
            u":"_s);
    }

    return pixmap;
}

QString Paint::getNametagPixmapCacheKey(const QString &text, const QFont &font,
                                        QColor userColor, QSizeF textSize,
                                        float scale, float dpr,
                                        QMarginsF margins) const
{
    return this->getPixmapCacheKey(text, font, userColor, textSize, scale,
                                   dpr) +
           u"/m" + QString::number(qRound64(margins.left() * dpr)) + u',' +
           QString::number(qRound64(margins.top() * dpr)) + u',' +
           QString::number(qRound64(margins.right() * dpr)) + u',' +
           QString::number(qRound64(margins.bottom() * dpr));
}

QPixmap Paint::croppedNametagPixmap(const QString &text, const QFont &font,
                                    QColor userColor, QSizeF textSize,
                                    float scale, float dpr,
                                    QMarginsF margins) const
{
    const auto padding = this->overflow(scale);
    const auto full = this->getPixmap(text, font, userColor, textSize, scale,
                                      dpr, false, padding);
    const QRectF part(padding - margins.left(), padding - margins.top(),
                      textSize.width() + margins.left() + margins.right(),
                      textSize.height() + margins.top() + margins.bottom());
    QPixmap pixmap((part.size() * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.drawPixmap(QPointF(-part.left(), -part.top()), full);
    return pixmap;
}

QColor Paint::overlayColors(QColor background, QColor foreground)
{
    auto alpha = foreground.alphaF();

    auto r = ((1 - alpha) * static_cast<float>(background.red())) +
             (alpha * static_cast<float>(foreground.red()));
    auto g = ((1 - alpha) * static_cast<float>(background.green())) +
             (alpha * static_cast<float>(foreground.green()));
    auto b = ((1 - alpha) * static_cast<float>(background.blue())) +
             (alpha * static_cast<float>(foreground.blue()));

    return {static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
}

qreal Paint::offsetRepeatingStopPosition(const qreal position,
                                         const QGradientStops &stops)
{
    const qreal gradientStart = stops.first().first;
    const qreal gradientEnd = stops.last().first;
    const qreal gradientLength = gradientEnd - gradientStart;
    const qreal offsetPosition = (position - gradientStart) / gradientLength;

    return offsetPosition;
}

}  // namespace chatterino
