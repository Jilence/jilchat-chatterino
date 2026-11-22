// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/NotebookEnums.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace chatterino {

namespace liquidglass {

/// Whether tabs at `location` are drawn as liquid glass (setting on and a
/// horizontal tab strip).
bool tabsEnabled(NotebookTabLocation location);

/// Whether split banners float over the chat as rounded cards.
bool floatingBanners();

/// Whether the chat input floats over the chat.
bool floatingInput();

/// How milky the glass is, 0 (clear) .. 1 (milky).
float frost();

/// Paints a floating banner card over the whole of `rect`: rounded,
/// slightly see-through `background` and a hairline rim. Fallback for
/// paintGlassCard while there is nothing to refract yet.
void paintFloatingCard(QPainter &painter, const QRectF &rect,
                       QColor background, qreal scale, bool lightTheme);

/// What is under `rect` (in `widget`'s coordinates) in the window this
/// frame, read from the backing store; only meaningful inside the widget's
/// paintEvent, where everything below it has just been painted. Null if the
/// window does not paint into an image.
QImage grabBehind(const QWidget *widget, const QRect &rect);

/// What `window` (a top-level of this app) currently shows in `rect`
/// (window coordinates), read from its backing store. Parts outside the
/// window are `outside`.
QImage grabFrom(const QWidget *window, const QRect &rect,
                const QColor &outside);

/// Paints `glass` (a top-level window of this app, from its paintEvent) as
/// liquid glass over `behind`, another window of this app it floats over:
/// synchronous with what `behind` painted, dark where it is not behind.
/// `radius` is in physical px.
void paintGlassWindow(QPainter &painter, QWidget *glass, QWidget *behind,
                      QColor background, qreal scale, bool lightTheme,
                      qreal radius);

/// Paints `target` (from its paintEvent) as a liquid glass card tinted with
/// `background`: what is under it is frosted and bent at the rim. `radius`
/// is in device-independent px before scaling.
void paintGlassCard(QPainter &painter, QWidget *target, QColor background,
                    qreal scale, bool lightTheme, qreal radius = 10);

/// Corner radius of the selection lens, in device-independent px.
constexpr qreal TAB_RADIUS = 4;

/// The lens over a tab's rect, leaving a hairline gap to its neighbours.
inline QRectF tabTileRect(const QRectF &tabRect, qreal scale)
{
    return tabRect.adjusted(0.75 * scale, 1.0 * scale, -0.75 * scale,
                            -1.0 * scale);
}

/// Lens parameters in device-independent px.
struct Optics {
    /// Width of the curved rim.
    float bezel = 9;
    /// Glass height; thicker glass bends more.
    float thickness = 16;
    /// Strength of the specular rim, 0..1.
    float specular = 0.4F;
    /// >1 magnifies the content under the flat top.
    float zoom = 1.0F;
    /// 0..1: the lens is lifted (moving); it brightens and the rim glows.
    float lift = 0;
    /// Colour the glass is tinted with and how much (0..1). A negative
    /// amount means clear glass: a little white frost.
    QColor tint = Qt::white;
    float tintAmount = -1;
};

/// Refracts `source` through a rounded-rect lens of the same size, the way
/// jiltv-frontend's liquid-glass.ts does it: a convex squircle rim (IOR 1.5)
/// whose Snell's-law shift bends the content under it, plus a specular rim.
/// `radius` and `pxScale` (scale * devicePixelRatio) map the optics to
/// physical pixels. Returns a premultiplied image of the same size.
QImage refract(const QImage &source, qreal radius, qreal pxScale,
               const Optics &optics, bool lightTheme);

/// refract() for a lens of `outSize` (physical px) sampling `source`, which
/// may be a scaled copy (`sourceScale` = source size / out size): sampling a
/// scaled down copy frosts the glass for free.
QImage refractScaled(const QImage &source, QSize outSize, float sourceScale,
                     qreal radius, qreal pxScale, const Optics &optics,
                     bool lightTheme);

}  // namespace liquidglass

/// The selection lens of a liquid glass tab strip. It lives on top of the
/// notebook's tabs and refracts what is painted under it each frame.
/// Moves between tabs on springs; the leading edge is stiffer than the
/// trailing one, so the lens stretches like a drop while it travels.
class GlassLens : public QWidget
{
public:
    explicit GlassLens(QWidget *notebook);

    /// `rect` is in notebook coordinates.
    void setTarget(const QRectF &rect, bool animate);
    void setScale(float scale);
    /// Corner radius in device-independent px.
    void setRadius(qreal radius);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Spring {
        double pos = 0;
        double vel = 0;
        double target = 0;
        double stiffness = 260;
        double damping = 24;

        void step(double dt);
        bool settled() const;
    };

    void tick();
    void applyGeometry();
    QRectF currentRect() const;

    Spring left_;
    Spring right_;
    Spring top_;
    Spring bottom_;
    /// Current lens rect in notebook coordinates.
    QRectF lensRect_;
    double lift_ = 0;
    float scale_ = 1;
    qreal radius_ = 6;
    bool initialized_ = false;

    /// The last glass painted and what it was made from.
    QImage glass_;
    QImage lastSource_;
    double lastLift_ = -1;
    bool lastLight_ = false;

    QTimer timer_;
    QElapsedTimer clock_;
};

}  // namespace chatterino
