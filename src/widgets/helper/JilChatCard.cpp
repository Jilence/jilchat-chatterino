// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/JilChatCard.hpp"

#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace chatterino::jilchatcard {

namespace {

/// The same few numbers each frame, so the sparks keep their places
double hashed(int index, int salt)
{
    const auto value =
        std::sin((double(index) * 12.9898) + (double(salt) * 78.233)) *
        43758.5453;
    return value - std::floor(value);
}

}  // namespace

QFont font(int pixels, bool strong)
{
    QFont result(strong ? QStringLiteral("Segoe UI Semibold")
                        : QStringLiteral("Segoe UI"));
    result.setPixelSize(pixels);
    if (strong)
    {
        result.setWeight(QFont::DemiBold);
    }
    return result;
}

const QPixmap &logo()
{
    static const QPixmap pixmap(QStringLiteral(":/icon.png"));
    return pixmap;
}

void drawCard(QPainter &p, const QRectF &card, double seconds,
              QPointF glowCenter, double glowRadius)
{
    p.save();

    // Soft drop shadow
    p.setPen(Qt::NoPen);
    for (int i = SHADOW; i > 0; i -= 2)
    {
        p.setBrush(QColor(0, 0, 0, 10));
        const auto r = card.adjusted(-i, -i + 5, i, i + 5);
        p.drawRoundedRect(r, RADIUS + i, RADIUS + i);
    }

    QPainterPath path;
    path.addRoundedRect(card, RADIUS, RADIUS);

    QLinearGradient background(card.topLeft(), card.bottomLeft());
    background.setColorAt(0, QColor(28, 28, 31));
    background.setColorAt(1, INK);
    p.fillPath(path, background);

    p.setClipPath(path);

    // A white glow, breathing slowly
    const double breath = 0.5 + 0.5 * std::sin(seconds * 1.3);
    QRadialGradient glow(glowCenter, glowRadius);
    glow.setColorAt(0, QColor(255, 255, 255, 26 + int(16 * breath)));
    glow.setColorAt(1, QColor(255, 255, 255, 0));
    p.fillRect(card, glow);

    // Sparks rising from the bottom
    for (int i = 0; i < 32; ++i)
    {
        const double speed = 7 + hashed(i, 1) * 20;
        const double size = 1 + hashed(i, 2) * 2.4;
        const double drift = 5 + hashed(i, 3) * 12;
        const double phase = hashed(i, 4) * 6.28;
        const double travel = card.height() + 12;
        const double y =
            card.bottom() + 6 -
            std::fmod(seconds * speed + hashed(i, 5) * travel, travel);
        const double x = card.left() + hashed(i, 6) * card.width() +
                         std::sin(seconds * 0.8 + phase) * drift;
        const double life =
            std::clamp((y - card.top()) / card.height(), 0.0, 1.0);
        const int alpha = int(
            30 + 120 * life * (0.6 + 0.4 * std::sin(seconds * 2.2 + phase)));
        p.setBrush(QColor(255, 255, 255, std::clamp(alpha, 0, 255)));
        p.drawEllipse(QRectF(x, y, size, size));
    }

    // Thin bright line along the top edge
    QLinearGradient edge(card.topLeft(), card.topRight());
    edge.setColorAt(0, QColor(255, 255, 255, 0));
    edge.setColorAt(0.3, QColor(255, 255, 255, 150));
    edge.setColorAt(0.7, QColor(255, 255, 255, 40));
    edge.setColorAt(1, QColor(255, 255, 255, 0));
    p.fillRect(QRectF(card.left(), card.top(), card.width(), 1.5), edge);

    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 34), 1));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);

    p.restore();
}

void drawLogo(QPainter &p, const QRectF &area, double seconds, double opacity)
{
    const double dpr =
        p.device() != nullptr ? p.device()->devicePixelRatioF() : 1.0;
    const QSize pixels = (area.size() * dpr).toSize();
    if (pixels.isEmpty())
    {
        return;
    }

    QImage layer =
        logo()
            .scaled(pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .toImage()
            .convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Darkens a moving diagonal band, only where the logo has pixels
    const double progress = std::fmod(seconds, 2.4) / 2.4;
    const int w = layer.width();
    const double center = -0.6 * w + progress * 2.2 * w;
    const double band = 0.42 * w;
    for (int y = 0; y < layer.height(); ++y)
    {
        auto *row = reinterpret_cast<QRgb *>(layer.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const double distance = std::abs((x + y * 0.4) - center) / band;
            if (distance >= 1)
            {
                continue;
            }
            const double keep = 1 - 0.55 * (1 - distance) * (1 - distance);
            const QRgb c = row[x];
            row[x] = qRgba(int(qRed(c) * keep), int(qGreen(c) * keep),
                           int(qBlue(c) * keep), qAlpha(c));
        }
    }
    layer.setDevicePixelRatio(dpr);

    p.save();
    p.setOpacity(p.opacity() * opacity);
    const QSizeF shown = QSizeF(layer.size()) / dpr;
    p.drawImage(
        QRectF(area.center() - QPointF(shown.width() / 2, shown.height() / 2),
               shown),
        layer);
    p.restore();
}

void drawButton(QPainter &p, const QRectF &area, const QString &text,
                bool primary, double hover, bool busy, double seconds)
{
    p.save();
    QPainterPath path;
    path.addRoundedRect(area, 9, 9);
    p.setPen(Qt::NoPen);

    if (primary)
    {
        // A soft glow that grows under the mouse
        for (int i = 4; i > 0; --i)
        {
            QColor glow(255, 255, 255);
            glow.setAlphaF(0.03 + 0.05 * hover);
            p.setBrush(glow);
            const auto outer =
                area.adjusted(-i * 1.5, -i * 1.5 + 1, i * 1.5, i * 1.5 + 1);
            p.drawRoundedRect(outer, 9 + i * 1.5, 9 + i * 1.5);
        }
        QLinearGradient fill(area.topLeft(), area.bottomLeft());
        fill.setColorAt(0, QColor(236 + int(14 * hover), 236 + int(14 * hover),
                                  238 + int(12 * hover)));
        fill.setColorAt(1, QColor(190 + int(20 * hover), 190 + int(20 * hover),
                                  196 + int(20 * hover)));
        p.fillPath(path, fill);
    }
    else
    {
        p.fillPath(path, QColor(255, 255, 255, 12 + int(20 * hover)));
        p.setPen(QPen(QColor(255, 255, 255, 30 + int(30 * hover)), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
    }

    if (busy)
    {
        // A light passing along the button, since it can take a little while
        const double phase = std::fmod(seconds / 1.1, 1.0);
        const double x = area.left() - 50 + phase * (area.width() + 100);
        QLinearGradient light(x, 0, x + 50, 0);
        light.setColorAt(0, QColor(255, 255, 255, 0));
        light.setColorAt(0.5, QColor(255, 255, 255, primary ? 160 : 60));
        light.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillPath(path, light);
    }

    p.setPen(primary ? INK : TEXT);
    p.setFont(font(13, true));
    p.drawText(area, Qt::AlignCenter, text);
    p.restore();
}

void drawClose(QPainter &p, const QRectF &area, double hover)
{
    p.save();
    if (hover > 0.01)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, int(26 * hover)));
        p.drawRoundedRect(area, 7, 7);
    }
    const int tone = MUTED.red() + int(90 * hover);
    p.setPen(
        QPen(QColor(tone, tone, tone + 2), 1.5, Qt::SolidLine, Qt::RoundCap));
    const auto c = area.center();
    p.drawLine(c + QPointF(-4.5, -4.5), c + QPointF(4.5, 4.5));
    p.drawLine(c + QPointF(4.5, -4.5), c + QPointF(-4.5, 4.5));
    p.restore();
}

void drawPill(QPainter &p, const QRectF &area, const QString &text,
              const QColor &textColor, const QColor &fill, const QColor &border)
{
    p.save();
    p.setPen(QPen(border, 1));
    p.setBrush(fill);
    p.drawRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), area.height() / 2,
                      area.height() / 2);
    p.setFont(font(10, true));
    p.setPen(textColor);
    p.drawText(area, Qt::AlignCenter, text);
    p.restore();
}

}  // namespace chatterino::jilchatcard
