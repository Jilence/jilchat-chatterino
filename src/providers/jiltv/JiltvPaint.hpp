// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QCache>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

namespace jiltv {

/// Straight (not premultiplied) sRGB, channels 0-1.
struct Rgba {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 1;
};

enum class Space : std::uint8_t { Oklch, Oklab, Srgb };
enum class Repeat : std::uint8_t { None, Repeat, Mirror };

struct Stop {
    Rgba color;
    double pos = 0;
};

struct Layer {
    QString id;
    QString kind;  // solid, linear, radial, conic, pattern, shader
    double opacity = 1;
    QString blend = QStringLiteral("normal");
    bool hidden = false;

    Rgba color;  // solid
    std::vector<Stop> stops;
    Space space = Space::Oklch;
    bool longerHue = false;
    Repeat repeat = Repeat::None;
    double scale = 100;
    double offset = 0;
    double angle = 0;
    double x = 50;
    double y = 50;
    double rx = 50;
    double ry = 50;

    QString pattern;  // stripes, dots, checker
    Rgba a;
    Rgba b;
    double size = 8;

    QString effect;  // shader
    std::vector<Rgba> colors;
    QString asset;
};

struct Shadow {
    double x = 0;
    double y = 0;
    double blur = 0;
    Rgba color;
};

struct Key {
    double t = 0;
    double num = 0;
    std::optional<Rgba> color;
    QString ease = QStringLiteral("linear");
};

struct Track {
    QString target;
    std::vector<Key> keys;
};

struct Doc {
    double duration = 4;
    /// Tile width in CSS px, or 0 to stretch over the name.
    double tileWidth = 0;
    std::vector<Layer> layers;
    std::vector<Shadow> shadows;
    std::optional<Shadow> stroke;  // x = width, color
    double hue = 0;
    double saturate = 1;
    double brightness = 1;
    std::vector<Track> tracks;
    Rgba fallback{1, 1, 1, 1};

    static Doc parse(const QJsonObject &json);
    /// The document with every track applied at loop time t (0-1).
    Doc frameAt(double t) const;
};

Rgba parseHex(const QString &hex);
QColor toQColor(const Rgba &c);
/// Mixes like a CSS gradient does (premultiplied, OKLCH hue fix-up).
Rgba mix(const Rgba &a, const Rgba &b, double t, Space space, bool longerHue);

}  // namespace jiltv

/// A JilTV paint: the layer document from the JilTV catalog, drawn the way
/// the browser extension draws it with CSS.
class JiltvPaint final : public Paint
{
public:
    JiltvPaint(QString id, QString name, const QJsonObject &data,
               const QString &cdn);

    QPixmap getPixmap(const QString &text, const QFont &font, QColor userColor,
                      QSizeF size, float scale, float dpr,
                      bool centerVertically, qreal padding) const override;

    QBrush asBrush(QColor userColor, QRectF drawingRect) const override;
    const std::vector<PaintDropShadow> &getDropShadows() const override;
    bool animated() const override;
    bool shadowsEnabled() const override;
    qreal overflow() const override;
    QString sourceName() const override;

    /// The paint's plain color, for places that can only show one.
    QColor fallbackColor() const;

private:
    QImage renderFill(const jiltv::Doc &doc, QSize px, double cssPx) const;
    QImage renderLayer(const jiltv::Layer &l, const jiltv::Doc &doc, QSize px,
                       double cssPx) const;

    jiltv::Doc doc_;
    /// Baked shader loops by layer id.
    std::unordered_map<QString, ImagePtr> loops_;
    bool hasTracks_ = false;

    struct Frame {
        qint64 step;
        QPixmap pixmap;
    };
    mutable QCache<QString, Frame> frames_{256};
};

}  // namespace chatterino
