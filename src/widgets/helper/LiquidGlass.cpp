// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/LiquidGlass.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QBackingStore>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <vector>
#include <cmath>

namespace chatterino {

namespace liquidglass {

namespace {

constexpr float IOR = 1.5F;
constexpr int SAMPLES = 128;

/// Convex squircle height profile: flat top, smooth roll-off at the edge.
float surface(float x)
{
    return std::pow(1.0F - std::pow(1.0F - x, 4.0F), 0.25F);
}

/// Inward shift (px) of the content under each sample of the rim, from the
/// outer edge (0) to the start of the flat top (1).
std::array<float, SAMPLES> refractionProfile(float bezel, float thickness)
{
    std::array<float, SAMPLES> out{};
    constexpr float d = 0.001F;
    for (int i = 0; i < SAMPLES; ++i)
    {
        const float x =
            std::clamp(float(i) / float(SAMPLES - 1), d, 1.0F - d);
        const float slope =
            ((surface(x + d) - surface(x - d)) / (2 * d)) * (thickness / bezel);
        const float incidence = std::atan(slope);
        const float refracted = std::asin(std::sin(incidence) / IOR);
        out[size_t(i)] =
            surface(x) * thickness * std::tan(incidence - refracted);
    }
    return out;
}

struct Distance {
    float dist;  // negative inside
    float nx;    // outward normal
    float ny;
};

Distance roundedRect(float px, float py, float hw, float hh, float r)
{
    const float qx = std::abs(px) - (hw - r);
    const float qy = std::abs(py) - (hh - r);
    const float sx = px < 0 ? -1.0F : 1.0F;
    const float sy = py < 0 ? -1.0F : 1.0F;
    if (qx > 0 && qy > 0)
    {
        const float len = std::hypot(qx, qy);
        return {len - r, (qx / len) * sx, (qy / len) * sy};
    }
    if (qx > qy)
    {
        return {qx - r, sx, 0};
    }
    return {qy - r, 0, sy};
}

struct Rgb {
    float r;
    float g;
    float b;
};

/// Bilinear sample of an opaque premultiplied image, clamped to its edges.
Rgb sample(const QImage &img, float x, float y)
{
    const int w = img.width();
    const int h = img.height();
    x = std::clamp(x, 0.0F, float(w - 1));
    y = std::clamp(y, 0.0F, float(h - 1));
    const int x0 = int(x);
    const int y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0);
    const float fy = y - float(y0);

    const auto *row0 = reinterpret_cast<const QRgb *>(img.constScanLine(y0));
    const auto *row1 = reinterpret_cast<const QRgb *>(img.constScanLine(y1));
    const QRgb p00 = row0[x0];
    const QRgb p10 = row0[x1];
    const QRgb p01 = row1[x0];
    const QRgb p11 = row1[x1];

    auto mix = [&](int (*channel)(QRgb)) {
        const float top = float(channel(p00)) * (1 - fx) + float(channel(p10)) * fx;
        const float bottom =
            float(channel(p01)) * (1 - fx) + float(channel(p11)) * fx;
        return (top * (1 - fy) + bottom * fy) / 255.0F;
    };
    return {mix(qRed), mix(qGreen), mix(qBlue)};
}

}  // namespace

bool tabsEnabled(NotebookTabLocation location)
{
    return getSettings()->liquidGlassTabs &&
           (location == NotebookTabLocation::Top ||
            location == NotebookTabLocation::Bottom);
}

bool floatingBanners()
{
    return getSettings()->floatingBanners;
}

bool floatingInput()
{
    return getSettings()->floatingInput;
}

float frost()
{
    return float(std::clamp(getSettings()->glassFrost.getValue(), 0, 100)) /
           100.0F;
}

void paintFloatingCard(QPainter &painter, const QRectF &rect,
                       QColor background, qreal scale, bool lightTheme)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal radius = 10 * scale;
    const QRectF card = rect.adjusted(0.5, 0.5, -0.5, -0.5);

    // See-through, so the chat shows behind it, but dense enough to read.
    background.setAlpha(lightTheme ? 225 : 218);
    painter.setPen(QPen(lightTheme ? QColor(0, 0, 0, 28)
                                   : QColor(255, 255, 255, 26),
                        1.0));
    painter.setBrush(background);
    painter.drawRoundedRect(card, radius, radius);
    painter.restore();
}

namespace {

/// Per-pixel optics of one lens shape: coverage, where to sample (relative
/// to the pixel) and the specular rim. Depends only on the shape, so it is
/// computed once per size and reused every frame.
struct LensGeometry {
    int width = 0;
    int height = 0;
    float radius = 0;
    float bezel = 0;
    float thickness = 0;
    float zoom = 0;
    float edge = 0;

    std::vector<uint8_t> coverage;  // 0..255
    std::vector<float> offsetX;     // sample at x + offsetX
    std::vector<float> offsetY;
    std::vector<float> rim;         // specular strength before scaling
    /// Rows/columns fully inside the flat top: no offset, no rim there.
    int flatTop = 0;
    int flatBottom = 0;
    int flatLeft = 0;
    int flatRight = 0;

    bool matches(int w, int h, float r, float bz, float th, float zm,
                 float ed) const
    {
        return w == this->width && h == this->height && r == this->radius &&
               bz == this->bezel && th == this->thickness &&
               zm == this->zoom && ed == this->edge;
    }
};

const LensGeometry &lensGeometry(int w, int h, float r, float bezel,
                                 float thickness, float zoom, float edge)
{
    // A few shapes are in use at once (lens, banners, usercards).
    static std::array<LensGeometry, 8> cache;
    static size_t next = 0;
    for (const auto &g : cache)
    {
        if (g.matches(w, h, r, bezel, thickness, zoom, edge))
        {
            return g;
        }
    }

    auto &g = cache[next];
    next = (next + 1) % cache.size();
    g = {};
    g.width = w;
    g.height = h;
    g.radius = r;
    g.bezel = bezel;
    g.thickness = thickness;
    g.zoom = zoom;
    g.edge = edge;
    const size_t n = size_t(w) * size_t(h);
    g.coverage.assign(n, 0);
    g.offsetX.assign(n, 0);
    g.offsetY.assign(n, 0);
    g.rim.assign(n, 0);

    const auto profile = refractionProfile(bezel, thickness);
    const float hw = float(w) / 2;
    const float hh = float(h) / 2;
    constexpr float lx = -0.6F;
    constexpr float ly = -0.8F;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float px = float(x) + 0.5F - hw;
            const float py = float(y) + 0.5F - hh;
            const auto [dist, nx, ny] = roundedRect(px, py, hw, hh, r);
            const float alpha = std::clamp(0.5F - dist, 0.0F, 1.0F);
            g.coverage[i] = uint8_t(alpha * 255 + 0.5F);
            if (alpha <= 0)
            {
                continue;
            }
            const float inside = std::max(-dist, 0.0F);
            float shift = 0;
            if (inside < bezel)
            {
                shift = profile[size_t(std::min(
                    SAMPLES - 1,
                    int(std::lround(inside / bezel * (SAMPLES - 1)))))];
            }
            // Sample further inward: the rim bends the content under it.
            g.offsetX[i] = px / zoom - px - nx * shift;
            g.offsetY[i] = py / zoom - py - ny * shift;

            const float facing = nx * lx + ny * ly;
            const float lit = facing > 0 ? facing : -facing * 0.45F;
            const float rim = std::exp(-inside / edge) +
                              0.18F * std::exp(-inside / (bezel * 0.45F));
            g.rim[i] = std::min(1.0F, rim * (0.25F + 0.75F * lit * lit));
        }
    }
    const int band = int(std::ceil(std::max(bezel, r) + 2));
    g.flatTop = band;
    g.flatBottom = h - band;
    g.flatLeft = band;
    g.flatRight = w - band;
    return g;
}

}  // namespace

QImage refractScaled(const QImage &source, QSize outSize, float sourceScale,
                     qreal radius, qreal pxScale, const Optics &optics,
                     bool lightTheme)
{
    const QImage src =
        source.format() == QImage::Format_ARGB32_Premultiplied
            ? source
            : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = outSize.width();
    const int h = outSize.height();
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    if (w < 2 || h < 2 || src.isNull())
    {
        out.fill(Qt::transparent);
        return out;
    }

    const float hw = float(w) / 2;
    const float hh = float(h) / 2;
    const float r = std::min({float(radius), hw, hh});
    const float bezel =
        std::max(1.0F, std::min({optics.bezel * float(pxScale), r, hw, hh}));
    const float thickness = optics.thickness * float(pxScale);
    const float zoom = std::max(0.5F, optics.zoom);
    const float lift = std::clamp(optics.lift, 0.0F, 1.0F);
    const float edge = 1.1F * float(pxScale);
    const auto &geometry =
        lensGeometry(w, h, r, bezel, thickness, zoom, edge);

    // Clear glass: a light frost of white (more in light themes), a hint of
    // saturation, brighter while lifted. Or tinted with the given colour.
    const bool clear = optics.tintAmount < 0;
    const float tintAmount =
        clear ? (lightTheme ? 0.3F : 0.12F) + lift * 0.05F
              : std::clamp(optics.tintAmount, 0.0F, 1.0F);
    const float tr = clear ? 1.0F : float(optics.tint.redF());
    const float tg = clear ? 1.0F : float(optics.tint.greenF());
    const float tb = clear ? 1.0F : float(optics.tint.blueF());
    const float saturation = 1.2F;
    const float specular = optics.specular + lift * 0.3F;
    const bool flatSampling = zoom == 1.0F && sourceScale == 1.0F;

    for (int y = 0; y < h; ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const bool rowFlat = y >= geometry.flatTop && y < geometry.flatBottom;
        const auto *srcLine =
            flatSampling && y < src.height()
                ? reinterpret_cast<const QRgb *>(src.constScanLine(y))
                : nullptr;
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const uint8_t coverage = geometry.coverage[i];
            if (coverage == 0)
            {
                line[x] = 0;
                continue;
            }

            float cr;
            float cg;
            float cb;
            const bool flat = rowFlat && x >= geometry.flatLeft &&
                              x < geometry.flatRight;
            if (flat && srcLine != nullptr && x < src.width())
            {
                // The flat top: nothing bends here.
                const QRgb p = srcLine[x];
                cr = float(qRed(p)) / 255.0F;
                cg = float(qGreen(p)) / 255.0F;
                cb = float(qBlue(p)) / 255.0F;
            }
            else
            {
                const auto rgb =
                    sample(src, (float(x) + geometry.offsetX[i]) * sourceScale,
                           (float(y) + geometry.offsetY[i]) * sourceScale);
                cr = rgb.r;
                cg = rgb.g;
                cb = rgb.b;
            }

            const float lum = 0.2126F * cr + 0.7152F * cg + 0.0722F * cb;
            cr = lum + (cr - lum) * saturation;
            cg = lum + (cg - lum) * saturation;
            cb = lum + (cb - lum) * saturation;
            cr += (tr - cr) * tintAmount;
            cg += (tg - cg) * tintAmount;
            cb += (tb - cb) * tintAmount;

            if (!flat)
            {
                const float a = geometry.rim[i] * specular;
                cr = 1 - (1 - std::clamp(cr, 0.0F, 1.0F)) * (1 - a);
                cg = 1 - (1 - std::clamp(cg, 0.0F, 1.0F)) * (1 - a);
                cb = 1 - (1 - std::clamp(cb, 0.0F, 1.0F)) * (1 - a);
            }
            const float alpha = float(coverage) / 255.0F;
            line[x] = qRgba(int(std::clamp(cr, 0.0F, 1.0F) * alpha * 255 + 0.5F),
                            int(std::clamp(cg, 0.0F, 1.0F) * alpha * 255 + 0.5F),
                            int(std::clamp(cb, 0.0F, 1.0F) * alpha * 255 + 0.5F),
                            int(coverage));
        }
    }
    return out;
}

QImage refract(const QImage &source, qreal radius, qreal pxScale,
               const Optics &optics, bool lightTheme)
{
    QImage out = refractScaled(source, source.size(), 1.0F, radius, pxScale,
                               optics, lightTheme);
    out.setDevicePixelRatio(source.devicePixelRatio());
    return out;
}

QImage grabBehind(const QWidget *widget, const QRect &rect)
{
    // Qt paints a window bottom-up into its backing store. By the time a
    // glass widget paints, everything under it in this frame is already
    // there: read it instead of rendering it again.
    auto *store = widget->window()->backingStore();
    if (store == nullptr)
    {
        return {};
    }
    auto *device = store->paintDevice();
    if (device == nullptr || device->devType() != QInternal::Image)
    {
        return {};
    }
    const auto *image = static_cast<const QImage *>(device);
    const qreal dpr = widget->devicePixelRatioF();
    const QPoint origin = widget->mapTo(widget->window(), rect.topLeft());
    const QRect pixels = QRect((QPointF(origin) * dpr).toPoint(),
                               (QSizeF(rect.size()) * dpr).toSize())
                             .intersected(image->rect());
    if (pixels.isEmpty())
    {
        return {};
    }
    QImage out = image->copy(pixels)
                     .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    return out;
}

QImage grabFrom(const QWidget *window, const QRect &rect,
                const QColor &outside)
{
    const qreal dpr = window->devicePixelRatioF();
    const QRect pixels((QPointF(rect.topLeft()) * dpr).toPoint(),
                       (QSizeF(rect.size()) * dpr).toSize());
    QImage out(pixels.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(outside);

    auto *store = window->backingStore();
    auto *device = store != nullptr ? store->paintDevice() : nullptr;
    if (device != nullptr && device->devType() == QInternal::Image &&
        window->isVisible() && !window->isMinimized())
    {
        const auto *image = static_cast<const QImage *>(device);
        const QRect visible = pixels.intersected(image->rect());
        if (!visible.isEmpty())
        {
            QPainter painter(&out);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(visible.topLeft() - pixels.topLeft(), *image,
                              visible);
        }
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

namespace {

/// The last glass a card painted, to skip the work when nothing under it
/// changed. Lives as a child of the card.
class CardCache : public QObject
{
public:
    using QObject::QObject;

    QImage source;
    QImage glass;
    QColor tint;
    qreal radius = 0;
    float frost = -1;
    bool light = false;
};

CardCache *cacheFor(QWidget *target)
{
    for (auto *child : target->children())
    {
        if (auto *cache = dynamic_cast<CardCache *>(child))
        {
            return cache;
        }
    }
    return new CardCache(target);
}

/// Frosts and refracts `under` for `target` and paints it, reusing the last
/// result when nothing changed. `radius` is in physical px.
void paintGlassFrom(QPainter &painter, QWidget *target, const QImage &under,
                    QColor background, qreal scale, bool lightTheme,
                    qreal radius)
{
    auto *cache = cacheFor(target);
    const qreal dpr = under.devicePixelRatio();
    const float milk = frost();
    if (under != cache->source || background != cache->tint ||
        radius != cache->radius || lightTheme != cache->light ||
        milk != cache->frost)
    {
        // Frost: sample a scaled down copy; bilinear sampling of it is the
        // blur. Clear glass samples the content as is.
        const QSize full = under.size();
        // Stepless: a factor of 1.25 is a hint of frost already.
        const qreal factor = 1.0 + qreal(milk) * 5.0 * scale * dpr;
        const QImage small =
            factor <= 1.01
                ? under
                : under.scaled(
                      std::max(1, int(std::round(full.width() / factor))),
                      std::max(1, int(std::round(full.height() / factor))),
                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        Optics optics;
        optics.bezel = 12;
        optics.thickness = 28;
        optics.specular = 0.45F;
        optics.tint = background;
        // Clear glass lets the chat through; milky glass hides it more.
        optics.tintAmount = (lightTheme ? 0.18F : 0.12F) + milk * 0.55F;
        cache->glass =
            refractScaled(small, full,
                          float(small.width()) / float(full.width()), radius,
                          scale * dpr, optics, lightTheme);
        cache->glass.setDevicePixelRatio(dpr);
        cache->source = under;
        cache->tint = background;
        cache->radius = radius;
        cache->frost = milk;
        cache->light = lightTheme;
    }
    painter.drawImage(QPointF(0, 0), cache->glass);
}

}  // namespace

void paintGlassCard(QPainter &painter, QWidget *target, QColor background,
                    qreal scale, bool lightTheme, qreal radius)
{
    const QImage under = grabBehind(target, target->rect());
    if (under.isNull())
    {
        paintFloatingCard(painter, target->rect(), background, scale,
                          lightTheme);
        return;
    }
    paintGlassFrom(painter, target, under, background, scale, lightTheme,
                   radius * scale * under.devicePixelRatio());
}

void paintGlassWindow(QPainter &painter, QWidget *glass, QWidget *behind,
                      QColor background, qreal scale, bool lightTheme,
                      qreal radius)
{
    const QRect rect(behind->mapFromGlobal(glass->mapToGlobal(QPoint(0, 0))),
                     glass->size());
    const QImage under = grabFrom(behind, rect, QColor(0, 0, 0));
    paintGlassFrom(painter, glass, under, background, scale, lightTheme,
                   radius);
}

}  // namespace liquidglass

// --------------------------------------------------------------- GlassLens

void GlassLens::Spring::step(double dt)
{
    const double force = -this->stiffness * (this->pos - this->target) -
                         this->damping * this->vel;
    this->vel += force * dt;
    this->pos += this->vel * dt;
}

bool GlassLens::Spring::settled() const
{
    return std::abs(this->pos - this->target) < 0.15 &&
           std::abs(this->vel) < 2.0;
}

GlassLens::GlassLens(QWidget *notebook)
    : QWidget(notebook)
{
    this->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->setAttribute(Qt::WA_NoSystemBackground);
    this->setFocusPolicy(Qt::NoFocus);
    this->hide();

    this->timer_.setTimerType(Qt::PreciseTimer);
    this->timer_.setInterval(16);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->tick();
    });
}

void GlassLens::setScale(float scale)
{
    this->scale_ = scale;
}

void GlassLens::setRadius(qreal radius)
{
    this->radius_ = radius;
}

QRectF GlassLens::currentRect() const
{
    return QRectF(QPointF(this->left_.pos, this->top_.pos),
                  QPointF(this->right_.pos, this->bottom_.pos));
}

void GlassLens::setTarget(const QRectF &rect, bool animate)
{
    this->left_.target = rect.left();
    this->right_.target = rect.right();
    this->top_.target = rect.top();
    this->bottom_.target = rect.bottom();

    if (!animate || !this->initialized_)
    {
        if (!this->timer_.isActive() || !this->initialized_)
        {
            for (auto *s : {&this->left_, &this->right_, &this->top_,
                            &this->bottom_})
            {
                s->pos = s->target;
                s->vel = 0;
            }
            this->lift_ = 0;
            this->initialized_ = true;
            this->applyGeometry();
        }
        // While animating, the springs simply follow the new target.
        return;
    }

    // The edge in the direction of travel leads, the other one trails: the
    // lens stretches on the way and snaps back into a pill when it lands.
    const bool towardsRight =
        rect.center().x() > this->currentRect().center().x();
    auto &leading = towardsRight ? this->right_ : this->left_;
    auto &trailing = towardsRight ? this->left_ : this->right_;
    leading.stiffness = 330;
    leading.damping = 26;
    trailing.stiffness = 150;
    trailing.damping = 20;
    this->top_.stiffness = this->bottom_.stiffness = 260;
    this->top_.damping = this->bottom_.damping = 26;

    if (!this->timer_.isActive())
    {
        this->clock_.restart();
        this->timer_.start();
    }
}

void GlassLens::tick()
{
    double dt = double(this->clock_.restart()) / 1000.0;
    // The GUI thread was busy (e.g. building the newly selected page): an
    // animation that far behind would only stutter, so land at once.
    const bool stalled = dt > 0.12;
    dt = std::clamp(dt, 0.0, 0.05);

    // Fixed substeps keep the springs stable at any frame rate.
    constexpr double sub = 0.004;
    for (double t = 0; t < dt; t += sub)
    {
        const double h = std::min(sub, dt - t);
        for (auto *s :
             {&this->left_, &this->right_, &this->top_, &this->bottom_})
        {
            s->step(h);
        }
    }

    // Lift with speed: the lens rises, magnifies and glows while it moves.
    const double speed = std::abs(this->left_.vel + this->right_.vel) / 2;
    const double liftTarget =
        std::clamp(speed / (700.0 * this->scale_), 0.0, 1.0);
    this->lift_ += (liftTarget - this->lift_) * std::min(1.0, dt * 16);

    const bool settled = stalled ||
                         (this->left_.settled() && this->right_.settled() &&
                          this->top_.settled() && this->bottom_.settled() &&
                          this->lift_ < 0.01);
    if (settled)
    {
        for (auto *s :
             {&this->left_, &this->right_, &this->top_, &this->bottom_})
        {
            s->pos = s->target;
            s->vel = 0;
        }
        this->lift_ = 0;
        this->timer_.stop();
    }
    this->applyGeometry();
}

void GlassLens::applyGeometry()
{
    const double s = this->scale_;
    const double grow = this->lift_ * 2.0 * s;
    this->lensRect_ = this->currentRect().normalized().adjusted(
        -grow, -grow * 0.6, grow, grow * 0.6);

    const int margin = int(std::ceil(6 * s));
    const QRect geometry = this->lensRect_.toAlignedRect().adjusted(
        -margin, -margin, margin, margin);
    if (geometry != this->geometry())
    {
        this->setGeometry(geometry);
    }
    this->update();
}

void GlassLens::paintEvent(QPaintEvent * /*event*/)
{
    const QRect lens =
        this->lensRect_.toAlignedRect().translated(-this->pos());
    if (lens.width() < 4 || lens.height() < 4)
    {
        return;
    }

    // The tabs under the lens, as painted into the window this very frame.
    const QImage under = liquidglass::grabBehind(this, lens);
    if (under.isNull())
    {
        return;
    }

    const bool light = getApp()->getThemes()->isLightTheme();
    const qreal s = this->scale_;
    const qreal radius = this->radius_ * s;
    if (under != this->lastSource_ || this->lift_ != this->lastLift_ ||
        light != this->lastLight_)
    {
        // At rest the lens is a thin, nearly flat sheet so the title stays
        // crisp; while it travels it thickens and bends the tabs under it.
        const auto lift = float(this->lift_);
        liquidglass::Optics optics;
        optics.lift = lift;
        optics.bezel = 5.0F + 4.0F * lift;
        optics.thickness = 5.0F + 17.0F * lift;
        optics.zoom = 1.0F + 0.16F * lift;
        const qreal dpr = under.devicePixelRatio();
        this->glass_ = liquidglass::refract(under, radius * dpr, s * dpr,
                                            optics, light);
        this->lastSource_ = under;
        this->lastLift_ = this->lift_;
        this->lastLight_ = light;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF local(lens);

    // Contact shadow, deeper while lifted.
    painter.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i)
    {
        const qreal g = i * (0.7 + this->lift_ * 0.5) * s;
        const QRectF shadow = local.adjusted(-g, -g * 0.3 + s, g, g + s);
        painter.setBrush(QColor(0, 0, 0, light ? 7 : 18));
        painter.drawRoundedRect(shadow, radius + g, radius + g);
    }

    painter.drawImage(local.topLeft(), this->glass_);
}

}  // namespace chatterino
