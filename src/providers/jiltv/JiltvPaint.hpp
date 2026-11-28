// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QCache>
#include <QJsonObject>
#include <QPainter>
#include <QString>

#include <array>
#include <atomic>
#include <memory>
#include <unordered_set>
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

/// A base picked from the user's color: onLight under light name colors,
/// onDark under dark ones (OKLCH lightness at least threshold, 0-100).
struct ContrastBase {
    Rgba onLight{0, 0, 0, 1};
    Rgba onDark{1, 1, 1, 1};
    double threshold = 60;
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
    /// Turns every gradient and stripe pattern, in degrees.
    double rotation = 0;
    std::vector<Track> tracks;
    Rgba fallback{1, 1, 1, 1};
    /// Contrast base instead of the user's color; absent: the user's color.
    std::optional<ContrastBase> base;

    static Doc parse(const QJsonObject &json);
    /// The document with every track applied at loop time t (0-1).
    Doc frameAt(double t) const;
};

Rgba parseHex(const QString &hex);
/// OKLCH lightness of a color, 0-100 (what a contrast base compares).
double lightness(const Rgba &c);
QColor toQColor(const Rgba &c);
/// Mixes like a CSS gradient does (premultiplied, OKLCH hue fix-up).
Rgba mix(const Rgba &a, const Rgba &b, double t, Space space, bool longerHue);

}  // namespace jiltv

/// A JilTV paint: the layer document from the JilTV catalog, drawn the way
/// the browser extension draws it with CSS.
///
/// A paint with day phases (`phases` in the document) has its own look for
/// the morning, midday, evening and night of the local time. Each such look
/// is a JiltvPaint of its own; this one hands every call to the look of the
/// current phase (updatePhase, called by the provider's timer).
class JiltvPaint final : public Paint
{
public:
    JiltvPaint(QString id, QString name, const QJsonObject &data,
               const QString &cdn, const QJsonObject &texture = {},
               const QJsonObject &phaseTextures = {});

    /// Picks the look for the current local time. True when it changed (the
    /// chat needs a repaint).
    bool updatePhase();
    /// Whether the paint has a look of its own for some day phase.
    bool hasPhases() const
    {
        return this->hasPhases_;
    }

    QPixmap getPixmap(const QString &text, const QFont &font, QColor userColor,
                      QSizeF size, float scale, float dpr,
                      bool centerVertically, qreal padding) const override;

    QBrush asBrush(QColor userColor, QRectF drawingRect) const override;
    const std::vector<PaintDropShadow> &getDropShadows() const override;
    bool animated() const override;
    /// A texture or baked shader loop: its pictures change with the GIF
    /// timer.
    bool animatesWithImages() const override
    {
        if (const auto *look = this->phaseLook())
        {
            return look->animatesWithImages();
        }
        return this->texture_ != nullptr || !this->loops_.empty();
    }
    bool shadowsEnabled() const override;
    qreal overflow(float scale) const override;
    QMarginsF overflowMargins(float scale) const override
    {
        if (const auto *look = this->phaseLook())
        {
            return look->overflowMargins(scale);
        }
        const auto room = this->overflow(scale);
        return {room, room, room, room};
    }
    QPixmap getNametagPixmap(const QString &text, const QFont &font,
                             QColor userColor, QSizeF textSize, float scale,
                             float dpr, QMarginsF margins) const override
    {
        if (const auto *look = this->phaseLook())
        {
            return look->getNametagPixmap(text, font, userColor, textSize,
                                          scale, dpr, margins);
        }
        // The chat asks for the whole glow room (overflowMargins), which is
        // exactly the pixmap getPixmap makes: hand that out as it is instead
        // of copying it into a new pixmap on every repaint.
        const auto room = this->overflow(scale);
        if (margins == QMarginsF(room, room, room, room))
        {
            return this->getPixmap(text, font, userColor, textSize, scale, dpr,
                                   false, room);
        }
        return this->croppedNametagPixmap(text, font, userColor, textSize,
                                          scale, dpr, margins);
    }
    std::optional<NametagLayers> getNametagLayers(
        const QString &text, const QFont &font, QColor userColor,
        QSizeF textSize, float scale, float dpr,
        QMarginsF margins) const override
    {
        if (const auto *look = this->phaseLook())
        {
            return look->getNametagLayers(text, font, userColor, textSize,
                                          scale, dpr, margins);
        }
        const auto room = this->overflow(scale);
        if (margins != QMarginsF(room, room, room, room))
        {
            return std::nullopt;
        }
        return this->layers(text, font, userColor, textSize, scale, dpr, false,
                            room);
    }
    QString sourceName() const override;

    /// The paint's plain color, for places that can only show one.
    QColor fallbackColor() const;

private:
    /// The look of the current day phase when it is not this one, else null.
    const JiltvPaint *phaseLook() const
    {
        const int phase = this->phase_.load(std::memory_order_relaxed);
        return phase >= 0 ? this->phases_[phase].get() : nullptr;
    }
    /// The color under the layers for the user's color (contrast base).
    QColor baseFor(QColor userColor) const;

    /// One background layer of the name: the user's color (`layer` null) or
    /// a paint layer, with its blend mode.
    struct Fill {
        const jiltv::Layer *layer = nullptr;
        QColor color;
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
        /// Whether its picture changes during the loop.
        bool moving = false;
    };
    /// The background layers of the name, bottom first: `base` (the user's
    /// color), then one per visible paint layer.
    std::vector<Fill> fills(const jiltv::Doc &doc, QColor base) const;
    /// A fill over the name's box, its opacity applied.
    QImage renderFill(const Fill &fill, const jiltv::Doc &doc, QSize px,
                      double cssPx) const;
    QImage renderLayer(const jiltv::Layer &l, const jiltv::Doc &doc, QSize px,
                       double cssPx) const;
    /// The name's picture at this moment as glow and letters. Every frame
    /// of the loop is drawn once and then kept, so a running animation only
    /// looks its pictures up.
    NametagLayers layers(const QString &text, const QFont &font,
                         QColor userColor, QSizeF size, float scale, float dpr,
                         bool centerVertically, qreal padding) const;

    jiltv::Doc doc_;
    /// Only local effects need timeline evaluation when the CDN texture is used.
    jiltv::Doc effectsDoc_;
    ImagePtr texture_;
    ImagePtr stillTexture_;
    bool textureTiles_ = false;
    /// Baked shader loops by layer id.
    std::unordered_map<QString, ImagePtr> loops_;
    bool hasTracks_ = false;
    /// Ids of the layers whose picture changes during the loop: animated by
    /// a track or a shader.
    std::unordered_set<QString> movingLayers_;
    /// Tells this paint's pictures apart from those of other paints (and of
    /// an earlier paint with the same id) in the shared cache.
    QString cacheId_;

    /// Own looks per day phase (morning, midday, evening, night); null:
    /// the phase shows this document.
    std::array<std::shared_ptr<JiltvPaint>, 4> phases_;
    /// Where each phase starts, in minutes after local midnight.
    std::array<int, 4> phaseStarts_{360, 660, 1020, 1260};
    bool hasPhases_ = false;
    /// Index of the phase whose own look is shown, or -1 for this one.
    std::atomic<int> phase_{-1};
};

}  // namespace chatterino
