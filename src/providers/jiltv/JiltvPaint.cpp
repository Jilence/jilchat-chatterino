// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jiltv/JiltvPaint.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QConicalGradient>
#include <QDateTime>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <ranges>

// The paint format is defined by @jiltv/paint (jiltv-paint/src/types.ts);
// this mirrors its CSS compiler (compile.ts), color math (color.ts) and
// timeline (timeline.ts) closely enough that a paint looks the same here as
// in the browser extension.

namespace chatterino {

using namespace Qt::Literals;

namespace jiltv {

namespace {

constexpr double PI = std::numbers::pi;

double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

double toLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double fromLinear(double c)
{
    return c <= 0.0031308
               ? 12.92 * c
               : (1.055 * std::copysign(std::pow(std::abs(c), 1 / 2.4), c)) -
                     0.055;
}

using Coords = std::array<double, 3>;

Coords rgbToOklab(const Rgba &c)
{
    const double lr = toLinear(c.r);
    const double lg = toLinear(c.g);
    const double lb = toLinear(c.b);
    const double l =
        std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
    const double m =
        std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
    const double s =
        std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
    return {
        0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
    };
}

Rgba oklabToRgb(double L, double A, double B, double alpha)
{
    const double l = std::pow(L + 0.3963377774 * A + 0.2158037573 * B, 3);
    const double m = std::pow(L - 0.1055613458 * A - 0.0638541728 * B, 3);
    const double s = std::pow(L - 0.0894841775 * A - 1.291485548 * B, 3);
    return {
        fromLinear(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
        fromLinear(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
        fromLinear(-0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s),
        alpha,
    };
}

// Below this chroma a color's hue doesn't count when mixing in OKLCH: the
// other color's hue is used. Chrome counts colors up to 0.02 as gray, far
// more than the spec (and color.ts) does; near-whites would otherwise turn
// orange or green halfway.
constexpr double ACHROMATIC = 0.02;

Coords toSpace(const Rgba &c, Space space)
{
    if (space == Space::Srgb)
    {
        return {c.r, c.g, c.b};
    }
    const auto lab = rgbToOklab(c);
    if (space == Space::Oklab)
    {
        return lab;
    }
    const double chroma = std::hypot(lab[1], lab[2]);
    const double h =
        chroma < ACHROMATIC
            ? std::nan("")
            : std::fmod(std::atan2(lab[2], lab[1]) * 180 / PI + 360, 360);
    return {lab[0], chroma, h};
}

Rgba fromSpace(const Coords &v, double alpha, Space space)
{
    if (space == Space::Srgb)
    {
        return {v[0], v[1], v[2], alpha};
    }
    if (space == Space::Oklab)
    {
        return oklabToRgb(v[0], v[1], v[2], alpha);
    }
    const double hr = (std::isnan(v[2]) ? 0 : v[2]) * PI / 180;
    return oklabToRgb(v[0], v[1] * std::cos(hr), v[1] * std::sin(hr), alpha);
}

Rgba clip(const Rgba &c)
{
    return {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
}

std::pair<double, double> fixHues(double h1, double h2, bool longer)
{
    if (std::isnan(h1) && std::isnan(h2))
    {
        return {0, 0};
    }
    if (std::isnan(h1))
    {
        return {h2, h2};
    }
    if (std::isnan(h2))
    {
        return {h1, h1};
    }
    const double d = h2 - h1;
    if (!longer)
    {
        if (d > 180)
        {
            h1 += 360;
        }
        else if (d < -180)
        {
            h2 += 360;
        }
    }
    else
    {
        if (d > 0 && d < 180)
        {
            h1 += 360;
        }
        else if (d > -180 && d <= 0)
        {
            h2 += 360;
        }
    }
    return {h1, h2};
}

// --- easing (timeline.ts) ---

double bezier(double x1, double y1, double x2, double y2, double x)
{
    const double cx = 3 * x1;
    const double bx = 3 * (x2 - x1) - cx;
    const double ax = 1 - cx - bx;
    const double cy = 3 * y1;
    const double by = 3 * (y2 - y1) - cy;
    const double ay = 1 - cy - by;
    const auto sx = [&](double t) {
        return ((ax * t + bx) * t + cx) * t;
    };
    const auto sy = [&](double t) {
        return ((ay * t + by) * t + cy) * t;
    };
    const auto dx = [&](double t) {
        return (3 * ax * t + 2 * bx) * t + cx;
    };
    double t = x;
    for (int i = 0; i < 8; i++)
    {
        const double err = sx(t) - x;
        if (std::abs(err) < 1e-6)
        {
            return sy(t);
        }
        const double d = dx(t);
        if (std::abs(d) < 1e-6)
        {
            break;
        }
        t -= err / d;
    }
    double lo = 0;
    double hi = 1;
    t = x;
    for (int i = 0; i < 30; i++)
    {
        const double v = sx(t);
        if (std::abs(v - x) < 1e-6)
        {
            break;
        }
        (v < x ? lo : hi) = t;
        t = (lo + hi) / 2;
    }
    return sy(t);
}

double ease(const QString &e, double x)
{
    if (e == u"step")
    {
        return x < 1 ? 0 : 1;
    }
    if (e == u"ease")
    {
        return bezier(0.25, 0.1, 0.25, 1, x);
    }
    if (e == u"in")
    {
        return bezier(0.42, 0, 1, 1, x);
    }
    if (e == u"out")
    {
        return bezier(0, 0, 0.58, 1, x);
    }
    if (e == u"in-out")
    {
        return bezier(0.42, 0, 0.58, 1, x);
    }
    if (e == u"back")
    {
        return bezier(0.34, 1.56, 0.64, 1, x);
    }
    return x;
}

/// A track's value at t: a number, or a color (mixed in OKLab like CSS).
Key valueAt(const std::vector<Key> &keys, double t)
{
    const auto &first = keys.front();
    const auto &last = keys.back();
    if (t <= first.t)
    {
        return first;
    }
    if (t >= last.t)
    {
        return last;
    }
    size_t i = 0;
    while (keys[i + 1].t < t)
    {
        i++;
    }
    const auto &a = keys[i];
    const auto &b = keys[i + 1];
    const double span = b.t - a.t;
    const double p = span > 0 ? ease(a.ease, (t - a.t) / span) : 1;
    Key out = a;
    if (a.color && b.color)
    {
        out.color = mix(*a.color, *b.color, clamp01(p), Space::Oklab, false);
    }
    else if (!a.color && !b.color)
    {
        out.num = a.num + (b.num - a.num) * p;
    }
    else if (p >= 0.5)
    {
        out = b;
    }
    return out;
}

// --- parsing ---

double num(const QJsonObject &o, const char *key, double fallback)
{
    const auto v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

Rgba hex(const QJsonObject &o, const char *key, const Rgba &fallback = {})
{
    const auto v = o.value(QLatin1String(key));
    return v.isString() ? parseHex(v.toString()) : fallback;
}

Layer parseLayer(const QJsonObject &o)
{
    Layer l;
    l.id = o.value("id").toString();
    l.kind = o.value("kind").toString();
    l.opacity = num(o, "opacity", 1);
    l.blend = o.value("blend").toString(QStringLiteral("normal"));
    l.hidden = o.value("hidden").toBool();
    l.color = hex(o, "color");
    for (const auto &s : o.value("stops").toArray())
    {
        const auto so = s.toObject();
        l.stops.push_back({hex(so, "color"), num(so, "pos", 0)});
    }
    const auto space = o.value("space").toString();
    l.space = space == u"srgb"    ? Space::Srgb
              : space == u"oklab" ? Space::Oklab
                                  : Space::Oklch;
    l.longerHue = o.value("hue").toString() == u"longer";
    const auto repeat = o.value("repeat").toString();
    l.repeat = repeat == u"repeat"   ? Repeat::Repeat
               : repeat == u"mirror" ? Repeat::Mirror
                                     : Repeat::None;
    l.scale = num(o, "scale", 100);
    l.offset = num(o, "offset", 0);
    l.angle = num(o, "angle", 0);
    l.x = num(o, "x", 50);
    l.y = num(o, "y", 50);
    l.rx = num(o, "rx", 50);
    l.ry = num(o, "ry", 50);
    l.pattern = o.value("pattern").toString();
    l.a = hex(o, "a");
    l.b = hex(o, "b");
    l.size = num(o, "size", 8);
    l.effect = o.value("effect").toString();
    for (const auto &c : o.value("colors").toArray())
    {
        l.colors.push_back(parseHex(c.toString()));
    }
    l.asset = o.value("asset").toString();
    return l;
}

/// Sets the property a track path names (targets.ts).
void setTarget(Doc &doc, const QString &path, const Key &v)
{
    const auto parts = path.split(u':');
    const auto setNum = [&](double &field) {
        if (!v.color)
        {
            field = v.num;
        }
    };
    const auto setColor = [&](Rgba &field) {
        if (v.color)
        {
            field = *v.color;
        }
    };
    if (parts.size() >= 3 && parts[0] == u"layer")
    {
        auto it = std::ranges::find_if(doc.layers, [&](const Layer &l) {
            return l.id == parts[1];
        });
        if (it == doc.layers.end())
        {
            return;
        }
        auto &l = *it;
        if (parts.size() == 5 && parts[2] == u"stop")
        {
            const auto i = parts[3].toUInt();
            if (i >= l.stops.size())
            {
                return;
            }
            if (parts[4] == u"color")
            {
                setColor(l.stops[i].color);
            }
            else if (parts[4] == u"pos")
            {
                setNum(l.stops[i].pos);
            }
            return;
        }
        const auto &p = parts[2];
        if (p == u"angle")
        {
            setNum(l.angle);
        }
        else if (p == u"offset")
        {
            setNum(l.offset);
        }
        else if (p == u"scale")
        {
            setNum(l.scale);
        }
        else if (p == u"x")
        {
            setNum(l.x);
        }
        else if (p == u"y")
        {
            setNum(l.y);
        }
        else if (p == u"rx")
        {
            setNum(l.rx);
        }
        else if (p == u"ry")
        {
            setNum(l.ry);
        }
        else if (p == u"size")
        {
            setNum(l.size);
        }
        else if (p == u"opacity")
        {
            setNum(l.opacity);
        }
        else if (p == u"color")
        {
            setColor(l.color);
        }
        else if (p == u"a")
        {
            setColor(l.a);
        }
        else if (p == u"b")
        {
            setColor(l.b);
        }
        return;
    }
    if (parts.size() == 3 && parts[0] == u"shadow")
    {
        const auto i = parts[1].toUInt();
        if (i >= doc.shadows.size())
        {
            return;
        }
        auto &s = doc.shadows[i];
        const auto &p = parts[2];
        if (p == u"x")
        {
            setNum(s.x);
        }
        else if (p == u"y")
        {
            setNum(s.y);
        }
        else if (p == u"blur")
        {
            setNum(s.blur);
        }
        else if (p == u"color")
        {
            setColor(s.color);
        }
        return;
    }
    if (parts.size() == 2 && parts[0] == u"stroke" && doc.stroke)
    {
        if (parts[1] == u"width")
        {
            setNum(doc.stroke->x);
        }
        else if (parts[1] == u"color")
        {
            setColor(doc.stroke->color);
        }
        return;
    }
    if (parts.size() == 2 && parts[0] == u"paint" && parts[1] == u"rotation")
    {
        setNum(doc.rotation);
        return;
    }
    if (parts.size() == 2 && parts[0] == u"filter")
    {
        if (parts[1] == u"hue")
        {
            setNum(doc.hue);
        }
        else if (parts[1] == u"saturate")
        {
            setNum(doc.saturate);
        }
        else if (parts[1] == u"brightness")
        {
            setNum(doc.brightness);
        }
    }
}

// --- drawing helpers ---

QPainter::CompositionMode compositionFor(const QString &blend)
{
    static const std::unordered_map<QString, QPainter::CompositionMode> modes{
        {QStringLiteral("multiply"), QPainter::CompositionMode_Multiply},
        {QStringLiteral("screen"), QPainter::CompositionMode_Screen},
        {QStringLiteral("overlay"), QPainter::CompositionMode_Overlay},
        {QStringLiteral("darken"), QPainter::CompositionMode_Darken},
        {QStringLiteral("lighten"), QPainter::CompositionMode_Lighten},
        {QStringLiteral("color-dodge"), QPainter::CompositionMode_ColorDodge},
        {QStringLiteral("color-burn"), QPainter::CompositionMode_ColorBurn},
        {QStringLiteral("hard-light"), QPainter::CompositionMode_HardLight},
        {QStringLiteral("soft-light"), QPainter::CompositionMode_SoftLight},
        {QStringLiteral("difference"), QPainter::CompositionMode_Difference},
        {QStringLiteral("exclusion"), QPainter::CompositionMode_Exclusion},
    };
    // hue, saturation, color and luminosity have no Qt equivalent.
    const auto it = modes.find(blend);
    return it == modes.end() ? QPainter::CompositionMode_SourceOver
                             : it->second;
}

struct PosColor {
    double pos;  // percent
    Rgba color;
};

/// The stops of a gradient layer as CSS places them: in order, never
/// behind the previous one, mirrored, scaled and offset.
std::vector<PosColor> placedStops(const Layer &l)
{
    std::vector<PosColor> stops;
    stops.reserve(l.stops.size() * 2);
    for (const auto &s : l.stops)
    {
        stops.push_back({s.pos, s.color});
    }
    if (l.repeat == Repeat::Mirror)
    {
        std::vector<PosColor> out;
        for (const auto &s : stops)
        {
            out.push_back({s.pos * 0.5, s.color});
        }
        for (auto it = stops.rbegin(); it != stops.rend(); ++it)
        {
            out.push_back({100 - (it->pos * 0.5), it->color});
        }
        stops = std::move(out);
    }
    for (auto &s : stops)
    {
        s.pos = (s.pos * l.scale * 0.01) + l.offset;
    }
    for (size_t i = 1; i < stops.size(); i++)
    {
        stops[i].pos = std::max(stops[i].pos, stops[i - 1].pos);
    }
    return stops;
}

/// Color of placed stops at p percent, mixed in the layer's space; pads
/// beyond the ends.
Rgba colorAt(const std::vector<PosColor> &stops, double p, const Layer &l)
{
    if (stops.empty())
    {
        return {};
    }
    if (p <= stops.front().pos)
    {
        return stops.front().color;
    }
    if (p >= stops.back().pos)
    {
        return stops.back().color;
    }
    size_t i = 0;
    while (stops[i + 1].pos < p)
    {
        i++;
    }
    const auto &a = stops[i];
    const auto &b = stops[i + 1];
    const double span = b.pos - a.pos;
    return span <= 0 ? b.color
                     : mix(a.color, b.color, (p - a.pos) / span, l.space,
                           l.longerHue);
}

/// Qt gradient stops for placed stops over [from, to] percent: the
/// original stops (hard edges stay hard) plus colors mixed in between.
QGradientStops qtStops(const std::vector<PosColor> &stops, double from,
                       double to, const Layer &l, bool reverse = false)
{
    QGradientStops out;
    const double span = to - from;
    if (span <= 0)
    {
        return out;
    }
    std::vector<double> at;
    constexpr int STEPS = 96;
    for (int i = 0; i <= STEPS; i++)
    {
        at.push_back(from + (span * i / STEPS));
    }
    std::vector<std::pair<double, Rgba>> points;
    for (double p : at)
    {
        points.emplace_back(p, colorAt(stops, p, l));
    }
    // Exact stops, both sides of a hard edge.
    for (size_t i = 0; i < stops.size(); i++)
    {
        const auto p = stops[i].pos;
        if (p > from && p < to)
        {
            points.emplace_back(p, stops[i].color);
        }
    }
    std::ranges::stable_sort(points, {}, &std::pair<double, Rgba>::first);
    double last = -1;
    for (const auto &[p, c] : points)
    {
        double t = std::clamp((p - from) / span, 0.0, 1.0);
        if (reverse)
        {
            t = 1 - t;
        }
        // Qt keeps both of two equal positions; nudge to keep them ordered.
        if (!reverse && t <= last)
        {
            t = std::min(1.0, last + 1e-6);
        }
        last = t;
        out.append({t, toQColor(c)});
    }
    if (reverse)
    {
        std::ranges::stable_sort(out, {}, &QGradientStop::first);
    }
    return out;
}

/// A tileable image of `src` repeated over `size`, starting at `origin`.
void fillTiled(QPainter &p, const QImage &tile, QRectF rect, QPointF origin)
{
    QBrush brush(tile);
    brush.setTransform(QTransform::fromTranslate(origin.x(), origin.y()));
    p.fillRect(rect, brush);
}

/// Three box blurs: close to a Gaussian with standard deviation `sigma`.
/// Blurs the shape in `img` and fills it with `color`, so it spreads beyond
/// the shape like a CSS drop-shadow
void blurAlpha(QImage &img, double sigma, const QColor &color)
{
    if (sigma < 0.5)
    {
        return;
    }
    // Broad glows contain no fine detail. Blur at quarter resolution instead
    // of running six full-size passes for every transparent animation frame.
    if (sigma >= 4 && img.width() >= 8 && img.height() >= 8)
    {
        const auto size = img.size();
        auto small =
            img.scaled(QSize((size.width() + 3) / 4, (size.height() + 3) / 4),
                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        blurAlpha(small, sigma / 4, color);
        img =
            small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return;
    }
    const int w = img.width();
    const int h = img.height();
    // Box sizes for three passes (W3C's approximation).
    const double ideal = std::sqrt((12 * sigma * sigma / 3) + 1);
    int lo = static_cast<int>(std::floor(ideal));
    if (lo % 2 == 0)
    {
        lo--;
    }
    const int hi = lo + 2;
    const int m = static_cast<int>(
        std::round(((12 * sigma * sigma) - (3 * lo * lo) - (12 * lo) - 9) /
                   (-4 * lo - 4)));
    std::vector<int> a(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++)
    {
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; x++)
        {
            a[(y * w) + x] = qAlpha(line[x]);
        }
    }
    std::vector<int> tmp(a.size());
    const auto pass = [&](int box, bool horizontal) {
        const int r = box / 2;
        const int len = horizontal ? w : h;
        const int lines = horizontal ? h : w;
        for (int line = 0; line < lines; line++)
        {
            const auto idx = [&](int i) {
                i = std::clamp(i, -1, len);
                if (i < 0 || i >= len)
                {
                    return -1;
                }
                return horizontal ? (line * w) + i : (i * w) + line;
            };
            int sum = 0;
            for (int i = -r; i <= r; i++)
            {
                const int k = idx(i);
                sum += k < 0 ? 0 : a[k];
            }
            for (int i = 0; i < len; i++)
            {
                tmp[idx(i)] = sum / box;
                const int add = idx(i + r + 1);
                const int rem = idx(i - r);
                sum += (add < 0 ? 0 : a[add]) - (rem < 0 ? 0 : a[rem]);
            }
        }
        a.swap(tmp);
    };
    for (int i = 0; i < 3; i++)
    {
        const int box = i < m ? lo : hi;
        pass(box, true);
        pass(box, false);
    }
    for (int y = 0; y < h; y++)
    {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; x++)
        {
            // The blurred alpha in the shadow's color, also where the shape
            // wasn't: that is where the glow shows
            const int na = a[(y * w) + x];
            line[x] = qPremultiply(
                qRgba(color.red(), color.green(), color.blue(), na));
        }
    }
}

/// CSS hue-rotate, saturate and brightness on a premultiplied image.
void applyFilters(QImage &img, double hue, double saturate, double brightness,
                  QRect bounds)
{
    if (hue == 0 && saturate == 1 && brightness == 1)
    {
        return;
    }
    const double c = std::cos(hue * PI / 180);
    const double s = std::sin(hue * PI / 180);
    const double h[9] = {
        0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715,
        0.072 - c * 0.072 + s * 0.928, 0.213 - c * 0.213 + s * 0.143,
        0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283,
        0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715,
        0.072 + c * 0.928 + s * 0.072,
    };
    const double v = saturate;
    const double sm[9] = {
        0.213 + 0.787 * v, 0.715 - 0.715 * v, 0.072 - 0.072 * v,
        0.213 - 0.213 * v, 0.715 + 0.285 * v, 0.072 - 0.072 * v,
        0.213 - 0.213 * v, 0.715 - 0.715 * v, 0.072 + 0.928 * v,
    };
    double m[9];
    for (int r = 0; r < 3; r++)
    {
        for (int col = 0; col < 3; col++)
        {
            m[(r * 3) + col] = 0;
            for (int k = 0; k < 3; k++)
            {
                m[(r * 3) + col] += sm[(r * 3) + k] * h[(k * 3) + col];
            }
            m[(r * 3) + col] *= brightness;
        }
    }
    bounds = bounds.intersected(img.rect());
    for (int y = bounds.top(); y <= bounds.bottom(); y++)
    {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = bounds.left(); x <= bounds.right(); x++)
        {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            if (a == 0)
            {
                continue;
            }
            // Premultiplied channels stay premultiplied under a linear map.
            const double r = qRed(px);
            const double g = qGreen(px);
            const double b = qBlue(px);
            const auto ch = [&](int row) {
                const double out = m[row * 3] * r + m[(row * 3) + 1] * g +
                                   m[(row * 3) + 2] * b;
                return std::clamp(static_cast<int>(std::lround(out)), 0, a);
            };
            line[x] = qRgba(ch(0), ch(1), ch(2), a);
        }
    }
}

QImage blank(QSize size)
{
    QImage img(size.expandedTo({1, 1}), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

}  // namespace

Rgba parseHex(const QString &text)
{
    QString h = text.startsWith(u'#') ? text.mid(1) : text;
    if (h.size() == 3 || h.size() == 4)
    {
        QString full;
        for (auto ch : h)
        {
            full += ch;
            full += ch;
        }
        h = full;
    }
    if (h.size() == 6)
    {
        h += u"ff";
    }
    bool ok = false;
    const auto v = h.toUInt(&ok, 16);
    if (!ok || h.size() != 8)
    {
        return {0, 0, 0, 1};
    }
    return {
        ((v >> 24) & 0xff) / 255.0,
        ((v >> 16) & 0xff) / 255.0,
        ((v >> 8) & 0xff) / 255.0,
        (v & 0xff) / 255.0,
    };
}

QColor toQColor(const Rgba &c)
{
    return QColor::fromRgbF(
        static_cast<float>(clamp01(c.r)), static_cast<float>(clamp01(c.g)),
        static_cast<float>(clamp01(c.b)), static_cast<float>(clamp01(c.a)));
}

Rgba mix(const Rgba &a, const Rgba &b, double t, Space space, bool longerHue)
{
    auto ca = toSpace(a, space);
    auto cb = toSpace(b, space);
    const bool polar = space == Space::Oklch;
    if (polar)
    {
        std::tie(ca[2], cb[2]) = fixHues(ca[2], cb[2], longerHue);
    }
    const auto pm = [polar](Coords c, double alpha) {
        c[0] *= alpha;
        c[1] *= alpha;
        if (!polar)
        {
            c[2] *= alpha;
        }
        return c;
    };
    const auto pa = pm(ca, a.a);
    const auto pb = pm(cb, b.a);
    const double alpha = a.a + (b.a - a.a) * t;
    if (alpha <= 0)
    {
        return {0, 0, 0, 0};
    }
    const auto lerp = [&](int i) {
        return pa[i] + (pb[i] - pa[i]) * t;
    };
    Coords out = {lerp(0) / alpha, lerp(1) / alpha,
                  polar ? lerp(2) : lerp(2) / alpha};
    return clip(fromSpace(out, alpha, space));
}

Doc Doc::parse(const QJsonObject &json)
{
    Doc d;
    d.duration = std::max(0.5, num(json, "duration", 4));
    const auto mapping = json.value("mapping").toObject();
    if (mapping.value("mode").toString() == u"tile")
    {
        d.tileWidth = std::max(1.0, num(mapping, "width", 120));
    }
    for (const auto &l : json.value("layers").toArray())
    {
        d.layers.push_back(parseLayer(l.toObject()));
    }
    for (const auto &s : json.value("shadows").toArray())
    {
        const auto so = s.toObject();
        d.shadows.push_back({num(so, "x", 0), num(so, "y", 0),
                             num(so, "blur", 0), hex(so, "color")});
    }
    if (const auto st = json.value("stroke"); st.isObject())
    {
        const auto so = st.toObject();
        d.stroke = Shadow{num(so, "width", 1), 0, 0, hex(so, "color")};
    }
    const auto filters = json.value("filters").toObject();
    d.hue = num(filters, "hue", 0);
    d.saturate = num(filters, "saturate", 1);
    d.brightness = num(filters, "brightness", 1);
    d.rotation = num(json, "rotation", 0);
    for (const auto &t : json.value("tracks").toArray())
    {
        const auto to = t.toObject();
        Track track{.target = to.value("target").toString(), .keys = {}};
        for (const auto &k : to.value("keys").toArray())
        {
            const auto ko = k.toObject();
            Key key{.t = num(ko, "t", 0),
                    .num = 0,
                    .color = std::nullopt,
                    .ease = ko.value("ease").toString(u"linear"_s)};
            const auto v = ko.value("v");
            if (v.isString())
            {
                key.color = parseHex(v.toString());
            }
            else
            {
                key.num = v.toDouble();
            }
            track.keys.push_back(key);
        }
        std::ranges::sort(track.keys, {}, &Key::t);
        if (track.keys.size() >= 2)
        {
            d.tracks.push_back(std::move(track));
        }
    }
    d.fallback = hex(json, "fallback", {1, 1, 1, 1});
    return d;
}

Doc Doc::frameAt(double t) const
{
    if (this->tracks.empty())
    {
        return *this;
    }
    Doc out = *this;
    for (const auto &track : this->tracks)
    {
        setTarget(out, track.target, valueAt(track.keys, t));
    }
    return out;
}

}  // namespace jiltv

using namespace jiltv;

JiltvPaint::JiltvPaint(QString id, QString name, const QJsonObject &data,
                       const QString &cdn, const QJsonObject &texture)
    : Paint(std::move(id), std::move(name))
    , doc_(Doc::parse(data))
    , hasTracks_(!this->doc_.tracks.empty())
{
    static quint64 instances = 0;
    this->cacheId_ = QString::number(++instances) + u':' + this->id;

    auto textureUrl = texture.value("url").toString();
    auto stillUrl = texture.value("still").toString();
    const auto hash = data.value("texture").toString();
    if (textureUrl.isEmpty() && !hash.isEmpty() && !cdn.isEmpty())
    {
        textureUrl = cdn + u"/paints/"_s + hash + u".webp"_s;
        stillUrl = cdn + u"/paints/"_s + hash + u"-0.webp"_s;
    }
    if (!textureUrl.isEmpty())
    {
        this->texture_ = Image::fromAutoscaledUrl(Url{textureUrl}, 128);
        this->texture_->setFrameCacheLifetime(std::chrono::seconds(30));
        if (!stillUrl.isEmpty())
        {
            this->stillTexture_ = Image::fromAutoscaledUrl(Url{stillUrl}, 128);
            this->stillTexture_->setFrameCacheLifetime(
                std::chrono::seconds(30));
        }
        this->textureTiles_ =
            texture.value("tile").toBool(this->doc_.tileWidth > 0);
    }
    this->effectsDoc_.duration = this->doc_.duration;
    this->effectsDoc_.shadows = this->doc_.shadows;
    this->effectsDoc_.stroke = this->doc_.stroke;
    this->effectsDoc_.hue = this->doc_.hue;
    this->effectsDoc_.saturate = this->doc_.saturate;
    this->effectsDoc_.brightness = this->doc_.brightness;
    for (const auto &track : this->doc_.tracks)
    {
        if (track.target.startsWith(u"shadow:") ||
            track.target.startsWith(u"stroke:") ||
            track.target.startsWith(u"filter:"))
        {
            this->effectsDoc_.tracks.push_back(track);
        }
    }

    bool rotating = false;
    for (const auto &track : this->doc_.tracks)
    {
        const auto parts = track.target.split(u':');
        if (parts.size() >= 3 && parts[0] == u"layer")
        {
            this->movingLayers_.insert(parts[1]);
        }
        else if (track.target == u"paint:rotation")
        {
            rotating = true;
        }
    }
    for (const auto &l : this->doc_.layers)
    {
        // The rotation turns gradients and patterns, not plain colors.
        if (l.kind == u"shader" || (rotating && l.kind != u"solid"))
        {
            this->movingLayers_.insert(l.id);
        }
        if (l.kind == u"shader" && !l.asset.isEmpty() && !cdn.isEmpty())
        {
            // Shader loops are backgrounds for a single line of text. Keep
            // their decoded frames at chat resolution, preserving aspect ratio.
            auto loop = Image::fromAutoscaledUrl(
                Url{cdn + u"/paints/"_s + l.asset + u".webp"_s}, 128);
            loop->setFrameCacheLifetime(std::chrono::seconds(30));
            this->loops_[l.id] = std::move(loop);
        }
    }
}

QImage JiltvPaint::renderLayer(const Layer &l, const Doc &doc, QSize px,
                               double cssPx) const
{
    QImage img = blank(px);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double w = px.width();
    const double h = px.height();
    const QRectF box(0, 0, w, h);

    // The paint's rotation turns every angle and moves radial and conic
    // centers around the middle (compile.ts: turned(), center()).
    const double angle = l.angle + doc.rotation;
    const auto center = [&]() {
        const double r = doc.rotation * PI / 180;
        const double dx = l.x - 50;
        const double dy = l.y - 50;
        return QPointF(
            w * (50 + (dx * std::cos(r)) - (dy * std::sin(r))) / 100,
            h * (50 + (dx * std::sin(r)) + (dy * std::cos(r))) / 100);
    };
    const auto linearLine = [&](double angleDeg) {
        const double a = angleDeg * std::numbers::pi / 180;
        const QPointF d(std::sin(a), -std::cos(a));
        const double len = std::abs(w * d.x()) + std::abs(h * d.y());
        const QPointF start = box.center() - (d * (len / 2));
        return std::tuple{start, d, len};
    };

    if (l.kind == u"solid")
    {
        p.fillRect(box, toQColor(l.color));
    }
    else if (l.kind == u"linear" || l.kind == u"radial" || l.kind == u"conic")
    {
        const auto stops = placedStops(l);
        if (stops.empty())
        {
            return img;
        }
        const bool repeat = l.repeat != Repeat::None;
        const double first = stops.front().pos;
        const double last = stops.back().pos;
        if (last - first < 1e-6)
        {
            p.fillRect(box, toQColor(stops.back().color));
            return img;
        }
        if (l.kind == u"linear")
        {
            const auto [start, d, len] = linearLine(angle);
            QLinearGradient g(start + (d * (len * first / 100)),
                              start + (d * (len * last / 100)));
            g.setStops(qtStops(stops, first, last, l));
            g.setSpread(repeat ? QGradient::RepeatSpread
                               : QGradient::PadSpread);
            p.fillRect(box, g);
        }
        else if (l.kind == u"radial")
        {
            // A circle of radius 1 in a space stretched to the ellipse.
            const double from = std::max(0.0, first);
            QRadialGradient g(QPointF(0, 0), last / 100, QPointF(0, 0),
                              from / 100);
            g.setStops(qtStops(stops, from, last, l));
            g.setSpread(repeat ? QGradient::RepeatSpread
                               : QGradient::PadSpread);
            QBrush brush(g);
            QTransform t;
            const auto c = center();
            t.translate(c.x(), c.y());
            t.scale(std::max(1e-3, w * l.rx / 100),
                    std::max(1e-3, h * l.ry / 100));
            brush.setTransform(t);
            p.fillRect(box, brush);
        }
        else
        {
            // CSS turns clockwise from the top, Qt counter-clockwise from
            // the right: start at 90 - angle and run the stops backwards.
            // Qt can't repeat a conical gradient, so the repeats are laid
            // out over the full turn here.
            std::vector<PosColor> turn;
            const double period = last - first;
            for (int i = 0; i <= 360; i++)
            {
                double pos = i * 100.0 / 360;
                double q = pos;
                if (repeat)
                {
                    q = first +
                        std::fmod(std::fmod(pos - first, period) + period,
                                  period);
                }
                turn.push_back({pos, colorAt(stops, q, l)});
            }
            QConicalGradient g(center(), 90 - angle);
            QGradientStops qs;
            for (auto it = turn.rbegin(); it != turn.rend(); ++it)
            {
                qs.append({1 - (it->pos / 100), toQColor(it->color)});
            }
            g.setStops(qs);
            p.fillRect(box, g);
        }
    }
    else if (l.kind == u"pattern")
    {
        const double size = std::max(1.0, l.size * cssPx);
        const double shift = l.offset * size / 100;
        const auto a = toQColor(l.a);
        const auto b = toQColor(l.b);
        if (l.pattern == u"stripes")
        {
            const auto [start, d, len] = linearLine(angle);
            QLinearGradient g(start + (d * shift),
                              start + (d * (shift + size)));
            g.setStops({{0, a}, {0.5, a}, {0.5 + 1e-6, b}, {1, b}});
            g.setSpread(QGradient::RepeatSpread);
            p.fillRect(box, g);
        }
        else if (l.pattern == u"dots")
        {
            const int s = std::max(1, static_cast<int>(std::lround(size)));
            QImage tile = blank({s, s});
            {
                QPainter tp(&tile);
                const double r = size * 0.3;
                QRadialGradient g(QPointF(s / 2.0, s / 2.0), size * 0.75);
                const double edge = r / (size * 0.75);
                const double soft = (r + (0.75 * cssPx)) / (size * 0.75);
                g.setStops({{0, a},
                            {std::min(edge, 1.0), a},
                            {std::min(soft, 1.0), b},
                            {1, b}});
                tp.fillRect(QRectF(0, 0, s, s), g);
            }
            fillTiled(p, tile, box, {shift, (h - s) / 2});
        }
        else if (l.pattern == u"checker")
        {
            const int s = std::max(1, static_cast<int>(std::lround(size)));
            QImage tile = blank({2 * s, 2 * s});
            {
                QPainter tp(&tile);
                tp.fillRect(QRect(s, 0, s, s), a);  // top right
                tp.fillRect(QRect(s, s, s, s), b);  // bottom right
                tp.fillRect(QRect(0, s, s, s), a);  // bottom left
                tp.fillRect(QRect(0, 0, s, s), b);  // top left
            }
            fillTiled(p, tile, box, {shift, 0});
        }
    }
    else if (l.kind == u"shader")
    {
        std::optional<QPixmap> frame;
        if (const auto it = this->loops_.find(l.id); it != this->loops_.end())
        {
            frame = it->second->pixmapOrLoad();
        }
        if (frame && !frame->isNull())
        {
            p.drawPixmap(box, *frame, QRectF(frame->rect()));
        }
        else if (l.effect != u"sparkle" && l.colors.size() >= 2)
        {
            // Its palette left to right until the loop has loaded.
            Layer palette = l;
            palette.space = Space::Oklch;
            palette.longerHue = false;
            palette.repeat = Repeat::None;
            palette.scale = 100;
            palette.offset = 0;
            palette.stops.clear();
            const auto n = static_cast<double>(l.colors.size() - 1);
            for (size_t i = 0; i < l.colors.size(); i++)
            {
                palette.stops.push_back({l.colors[i], i * 100 / n});
            }
            const auto stops = placedStops(palette);
            QLinearGradient g(0, 0, w, 0);
            g.setStops(qtStops(stops, 0, 100, palette));
            p.fillRect(box, g);
        }
    }
    p.end();
    return img;
}

namespace {

/// Where the shadows of a name come from and what they look like. Casting
/// them (the blur) is the costly part, but it depends only on the outline of
/// the name and where the shadows lie, not on the colors that move.
struct ShadowCache {
    /// The alpha of the name they were cast from.
    QByteArray alpha;
    qint64 mask = 0;
    /// Offset, blur and opacity of each shadow, in order.
    std::vector<std::array<double, 4>> shapes;
    /// Each shadow's blurred alpha, as it lies under the name.
    std::vector<QByteArray> spread;
    /// The colors `under` was made with.
    std::vector<QRgb> colors;
    /// All shadows over each other, to draw the name on.
    QImage under;
};

/// What the pictures of one name share from frame to frame.
struct NameState {
    /// The stroke width the mask was made for; -1 without stroke.
    double maskStroke = -2;
    /// The letters (and stroke) in white: what the paint is clipped to.
    QImage mask;
    QPainterPath strokeOutline;

    bool fillsValid = false;
    /// The bottom fills, up to the first one that moves, clipped and
    /// blended.
    QImage stillBase;
    size_t stillCount = 0;
    /// The fills above, clipped; null for those that move.
    std::vector<QImage> clipped;

    ShadowCache shadows;

    qint64 step = -2;
    qint64 textureFrame = 0;
    QPixmap pixmap;
};

/// Shared by all paints, in KiB.
QCache<QString, NameState> &nameStates()
{
    static QCache<QString, NameState> cache(32 * 1024);
    return cache;
}

QByteArray alphaOf(const QImage &img)
{
    const int w = img.width();
    QByteArray out(static_cast<qsizetype>(w) * img.height(), Qt::Uninitialized);
    auto *dst = reinterpret_cast<uchar *>(out.data());
    for (int y = 0; y < img.height(); y++)
    {
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; x++)
        {
            *dst++ = static_cast<uchar>(qAlpha(line[x]));
        }
    }
    return out;
}

/// `alpha` (as from alphaOf) in one color, premultiplied.
QImage colored(const QByteArray &alpha, QSize size, QRgb color)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    const auto *src = reinterpret_cast<const uchar *>(alpha.constData());
    const int r = qRed(color);
    const int g = qGreen(color);
    const int b = qBlue(color);
    for (int y = 0; y < size.height(); y++)
    {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < size.width(); x++)
        {
            line[x] = qPremultiply(qRgba(r, g, b, *src++));
        }
    }
    return img;
}

void clipTo(QImage &img, const QImage &mask)
{
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask);
}

}  // namespace

std::vector<JiltvPaint::Fill> JiltvPaint::fills(const Doc &doc,
                                                QColor base) const
{
    std::vector<Fill> out;
    if (std::ranges::none_of(doc.layers, [](const Layer &l) {
            return !l.hidden;
        }))
    {
        out.push_back({.color = toQColor(doc.fallback)});
        return out;
    }
    out.push_back({.color = base});
    for (const auto &l : doc.layers)
    {
        if (l.hidden)
        {
            continue;
        }
        out.push_back({
            .layer = &l,
            .color = {},
            .mode = compositionFor(l.blend),
            .moving = this->movingLayers_.contains(l.id),
        });
    }
    return out;
}

QImage JiltvPaint::renderFill(const Fill &fill, const Doc &doc, QSize px,
                              double cssPx) const
{
    if (fill.layer == nullptr)
    {
        QImage img(px, QImage::Format_ARGB32_Premultiplied);
        img.fill(fill.color);
        return img;
    }
    const auto &l = *fill.layer;
    const QRectF box(QPointF(0, 0), QSizeF(px));
    const int tile =
        doc.tileWidth > 0
            ? std::max(1, static_cast<int>(std::lround(doc.tileWidth * cssPx)))
            : 0;
    QImage img = blank(px);
    QPainter p(&img);
    p.setOpacity(clamp01(l.opacity));
    // Dots and checkers have their own cell size; everything else is
    // sized by the mapping: the whole name, or one tile repeated.
    const bool ownSize = l.kind == u"pattern" &&
                         (l.pattern == u"dots" || l.pattern == u"checker");
    if (tile > 0 && !ownSize)
    {
        fillTiled(p, this->renderLayer(l, doc, {tile, px.height()}, cssPx), box,
                  {0, 0});
    }
    else
    {
        p.drawImage(QPointF(0, 0), this->renderLayer(l, doc, px, cssPx));
    }
    p.end();
    return img;
}

QPixmap JiltvPaint::getPixmap(const QString &text, const QFont &font,
                              QColor userColor, QSizeF size, float scale,
                              float dpr, bool centerVertically,
                              qreal padding) const
{
    const bool animate = getSettings()->jiltvAnimatedPaints;
    const bool effects = this->shadowsEnabled();
    std::optional<QPixmap> textureFrame;
    if (this->texture_)
    {
        const auto &source =
            animate && this->animated() ? this->texture_ : this->stillTexture_;
        if (source)
        {
            textureFrame = source->pixmapOrLoad();
        }
    }
    const bool baked = textureFrame && !textureFrame->isNull();
    const qint64 textureKey = baked ? textureFrame->cacheKey() : 0;
    const auto durationMs =
        static_cast<qint64>(std::lround(this->doc_.duration * 1000));
    // A new picture 30 times a second; all names move in lockstep.
    const qint64 step =
        animate && this->animated() &&
                (!baked || (effects && !this->effectsDoc_.tracks.empty()))
            ? (QDateTime::currentMSecsSinceEpoch() % durationMs) * 30 / 1000
            : -1;

    // The chat gives the name `padding` (overflow()) on every side for the
    // glow and draws the pixmap over that larger rect, so the pixmap must
    // include it, centered or not; otherwise the name gets stretched.
    padding = std::max<qreal>(0, padding);
    const auto key = QStringLiteral("%1|%2|%3|%4x%5|%6|%7|%8|%9|%10|%11")
                         .arg(this->cacheId_, text, font.key())
                         .arg(size.width())
                         .arg(size.height())
                         .arg(scale)
                         .arg(dpr)
                         .arg(centerVertically)
                         .arg(padding)
                         .arg(effects)
                         .arg(userColor.rgba());

    // Everything of the name that stays the same between frames is kept, so
    // a frame only draws what moves: the animated layers, and the shadows
    // in their current colors.
    auto &states = nameStates();
    NameState *state = states.object(key);
    if (state != nullptr && state->step == step &&
        state->textureFrame == textureKey && !state->pixmap.isNull())
    {
        return state->pixmap;
    }

    const double t =
        step < 0 ? 0 : static_cast<double>(step) * 1000 / 30 / durationMs;
    const Doc doc = (baked ? this->effectsDoc_ : this->doc_)
                        .frameAt(std::clamp(t, 0.0, 1.0));

    auto nametag = text;
    const bool colon = nametag.endsWith(u':');
    if (colon)
    {
        nametag.chop(1);
    }

    const QSizeF drawSize = size + QSizeF(2 * padding, 2 * padding);
    const QSize devSize = (drawSize * dpr).toSize().expandedTo({1, 1});
    const QRectF textRect(QPointF{padding, padding}, size);
    const QFontMetricsF metrics(font);
    const double baseline =
        centerVertically
            ? textRect.top() + ((textRect.height() - metrics.height()) / 2) +
                  metrics.ascent()
            : textRect.bottom() - metrics.descent();

    // The paint covers the name's box, like a CSS background on the name.
    const QRectF nameBox(textRect.left(), textRect.top(),
                         metrics.horizontalAdvance(nametag), textRect.height());
    const double cssPx = scale * dpr;  // device pixels per CSS pixel
    const QSize fillSize = (nameBox.size() * dpr).toSize().expandedTo({1, 1});
    const QPoint origin(static_cast<int>(std::floor(nameBox.left() * dpr)),
                        static_cast<int>(std::floor(nameBox.top() * dpr)));

    // Below the layers lies the user's own color, which shows where the
    // paint is (partly) transparent, as on the website.
    const auto base = userColor.isValid() ? userColor : this->fallbackColor();
    const auto fills = baked ? std::vector<Fill>{} : this->fills(doc, base);

    if (state == nullptr)
    {
        // Fills and masks only cover the name, not the much larger glow
        // margin. Overcharging every fill at frame size evicts visible names
        // and makes their shadows get rebuilt on every animation tick.
        const qint64 frameBytes =
            static_cast<qint64>(devSize.width()) * devSize.height() * 4;
        const qint64 fillBytes =
            static_cast<qint64>(fillSize.width()) * fillSize.height() * 4;
        const auto cost = static_cast<qsizetype>(
            (frameBytes * (effects ? 2 : 1) +
             fillBytes * static_cast<qint64>(fills.size() + 2) +
             (effects
                  ? frameBytes / 4 * static_cast<qint64>(doc.shadows.size() + 1)
                  : 0) +
             1023) /
            1024);
        if (states.insert(key, new NameState, std::max<qsizetype>(1, cost)))
        {
            state = states.object(key);
        }
    }
    // Too large to keep: drawn from scratch every time.
    NameState uncached;
    NameState &s = state != nullptr ? *state : uncached;

    const bool stroked = effects && doc.stroke && doc.stroke->x > 0;
    const double strokeWidth = stroked ? doc.stroke->x : -1;
    if (s.mask.isNull() || s.maskStroke != strokeWidth)
    {
        QPainterPath name;
        name.addText(textRect.left(), baseline, font, nametag);
        s.strokeOutline = {};
        if (stroked)
        {
            QPainterPathStroker stroker;
            stroker.setWidth(doc.stroke->x * scale);
            stroker.setJoinStyle(Qt::MiterJoin);
            stroker.setCapStyle(Qt::FlatCap);
            s.strokeOutline = stroker.createStroke(name);
        }

        // What a browser does with `background-clip: text`: the letters
        // (and their stroke) become a mask, and every background layer is
        // clipped to it on its own before it is blended onto those below.
        // Where a letter covers a pixel partly, the layers add up there, so
        // edges come out stronger than with a single clip; names with many
        // layers look bolder.
        s.mask = blank(fillSize);
        QPainter mp(&s.mask);
        mp.setRenderHint(QPainter::Antialiasing);
        mp.translate(-origin);
        mp.scale(dpr, dpr);
        if (stroked)
        {
            // The outline, as the stroke follows it; hinted letters are a
            // little wider and would hide part of the stroke.
            mp.fillPath(name, Qt::white);
            mp.fillPath(s.strokeOutline, Qt::white);
        }
        else
        {
            // Hinted, like the letters of names without a paint.
            mp.setPen(Qt::white);
            mp.setFont(font);
            mp.drawText(QPointF(textRect.left(), baseline), nametag);
        }
        mp.end();
        s.maskStroke = strokeWidth;
        s.fillsValid = false;
    }

    const auto clippedFill = [&](const Fill &fill) {
        QImage img = this->renderFill(fill, doc, fillSize, cssPx);
        clipTo(img, s.mask);
        return img;
    };
    if (!baked && !s.fillsValid)
    {
        // The fills below the first moving one always blend to the same.
        s.stillCount = 0;
        while (s.stillCount < fills.size() && !fills[s.stillCount].moving)
        {
            s.stillCount++;
        }
        s.stillBase = blank(fillSize);
        {
            QPainter bp(&s.stillBase);
            for (size_t i = 0; i < s.stillCount; i++)
            {
                bp.setCompositionMode(fills[i].mode);
                bp.drawImage(0, 0, clippedFill(fills[i]));
            }
        }
        s.clipped.assign(fills.size(), {});
        for (size_t i = s.stillCount; i < fills.size(); i++)
        {
            if (!fills[i].moving)
            {
                s.clipped[i] = clippedFill(fills[i]);
            }
        }
        s.fillsValid = true;
    }

    // A moving texture over an opaque base leaves the glyph shape unchanged.
    // Start with the cached glow and draw only the small name over it, avoiding
    // a second full-size transparent surface and a second composition pass.
    const bool reuseShadowUnder =
        baked && effects && base.alpha() == 255 && !stroked &&
        this->effectsDoc_.tracks.empty() && s.textureFrame != 0 &&
        s.shadows.mask == s.mask.cacheKey() && !s.shadows.under.isNull();
    QImage img = reuseShadowUnder ? s.shadows.under.copy() : blank(devSize);
    {
        QPainter p(&img);
        if (baked)
        {
            // The texture already contains all blends, filters and animation.
            // Composite over the user's color, then clip once to the glyphs.
            QImage painted = blank(fillSize);
            {
                QPainter pp(&painted);
                pp.fillRect(painted.rect(), base);
                pp.setRenderHint(QPainter::SmoothPixmapTransform);
                if (this->textureTiles_)
                {
                    const auto tileWidth = std::max(
                        1, qRound(static_cast<double>(textureFrame->width()) *
                                  fillSize.height() / textureFrame->height()));
                    const auto tile = textureFrame->scaled(
                        tileWidth, fillSize.height(), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
                    pp.fillRect(painted.rect(), QBrush(tile));
                }
                else
                {
                    pp.drawPixmap(painted.rect(), *textureFrame);
                }
            }
            clipTo(painted, s.mask);
            p.drawImage(origin, painted);
        }
        else if (s.stillCount == fills.size())
        {
            p.drawImage(origin, s.stillBase);
        }
        else
        {
            QImage painted = s.stillBase.copy();
            {
                QPainter pp(&painted);
                for (size_t i = s.stillCount; i < fills.size(); i++)
                {
                    pp.setCompositionMode(fills[i].mode);
                    pp.drawImage(
                        0, 0,
                        fills[i].moving ? clippedFill(fills[i]) : s.clipped[i]);
                }
            }
            p.drawImage(origin, painted);
        }
        // The text's own fill is transparent, so the whole stroke lies on
        // top of the paint, not only its outer half (paint-order doesn't
        // change that). Filled as an outline: Qt draws pens thinner than a
        // pixel fainter than the browser does.
        if (stroked)
        {
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(dpr, dpr);
            auto color = toQColor(doc.stroke->color);
            if (baked)
            {
                QImage swatch = blank({1, 1});
                swatch.fill(color);
                applyFilters(swatch, doc.hue, doc.saturate, doc.brightness,
                             swatch.rect());
                color = swatch.pixelColor(0, 0);
            }
            p.fillPath(s.strokeOutline, color);
        }
    }
    // Filters precede drop shadows. Only the letters and stroke have pixels
    // here; scanning the large transparent glow margin wastes most of the work.
    const int filterMargin =
        stroked ? static_cast<int>(std::ceil(doc.stroke->x * cssPx)) + 2 : 0;
    if (!baked)
    {
        applyFilters(img, doc.hue, doc.saturate, doc.brightness,
                     QRect(origin, fillSize)
                         .adjusted(-filterMargin, -filterMargin, filterMargin,
                                   filterMargin));
    }

    std::vector<const Shadow *> shadows;
    if (effects && !reuseShadowUnder)
    {
        for (const auto &shadow : doc.shadows)
        {
            if (shadow.color.a > 0)
            {
                shadows.push_back(&shadow);
            }
        }
    }
    if (!shadows.empty())
    {
        // CSS `filter: drop-shadow(a) drop-shadow(b)` chains: each shadow is
        // cast by the name together with the shadows before it, so a later
        // shadow also tints the glow of an earlier one. Where they lie
        // depends only on the outline of the name and on the shapes and
        // opacities of the shadows, so they are cast again only when one of
        // those changes; moving colors only tint them anew.
        auto &cache = s.shadows;
        std::vector<std::array<double, 4>> shapes;
        std::vector<QRgb> colors;
        for (const auto *shadow : shadows)
        {
            shapes.push_back({shadow->x, shadow->y, shadow->blur,
                              toQColor(shadow->color).alphaF()});
            colors.push_back(toQColor(shadow->color).rgba());
        }
        // With an opaque name color and no stroke, a baked texture cannot
        // change the glyph alpha. Reuse it without reading the entire glow
        // surface on every frame. Translucent colors/strokes still compare
        // the actual pixels so animated opacity remains correct.
        auto alpha =
            baked && s.textureFrame != 0 && cache.mask == s.mask.cacheKey() &&
                    base.alpha() == 255 && !stroked && !cache.alpha.isEmpty()
                ? cache.alpha
                : alphaOf(img);
        if (alpha != cache.alpha || shapes != cache.shapes)
        {
            cache.spread.clear();
            cache.colors.clear();
            QImage cast = img;
            for (const auto *shadow : shadows)
            {
                const auto shadowColor = toQColor(shadow->color);
                QImage layer = blank(devSize);
                {
                    QPainter sp(&layer);
                    sp.drawImage(QPointF(shadow->x * cssPx, shadow->y * cssPx),
                                 cast);
                    sp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    sp.fillRect(layer.rect(), shadowColor);
                }
                // Browsers take a drop-shadow's blur as the standard
                // deviation itself (box-shadow and text-shadow use half of
                // it).
                blurAlpha(layer, shadow->blur * cssPx, shadowColor);
                cache.spread.push_back(alphaOf(layer));
                {
                    QPainter sp(&layer);
                    sp.drawImage(0, 0, cast);
                }
                cast.swap(layer);
            }
            cache.alpha = std::move(alpha);
            cache.mask = s.mask.cacheKey();
            cache.shapes = std::move(shapes);
        }
        if (colors != cache.colors)
        {
            // The first shadow lies on top of the second, and so on.
            cache.under = blank(devSize);
            QPainter up(&cache.under);
            for (size_t i = cache.spread.size(); i-- > 0;)
            {
                up.drawImage(0, 0,
                             colored(cache.spread[i], devSize, colors[i]));
            }
            cache.colors = std::move(colors);
        }
        QImage out = cache.under.copy();
        {
            QPainter op(&out);
            op.drawImage(0, 0, img);
        }
        img.swap(out);
    }

    if (colon)
    {
        QPainter p(&img);
        p.scale(dpr, dpr);
        p.setPen(getApp()->getThemes()->messages.textColors.regular);
        p.setFont(font);
        p.drawText(QPointF(nameBox.right(), baseline), QStringLiteral(":"));
    }

    auto pixmap = QPixmap::fromImage(std::move(img));
    pixmap.setDevicePixelRatio(dpr);
    s.step = step;
    s.textureFrame = textureKey;
    s.pixmap = pixmap;
    return pixmap;
}

QBrush JiltvPaint::asBrush(QColor /*userColor*/, QRectF /*drawingRect*/) const
{
    // getPixmap draws the paint; this is only asked for a plain color.
    return {this->fallbackColor()};
}

const std::vector<PaintDropShadow> &JiltvPaint::getDropShadows() const
{
    // getPixmap draws the shadows itself, the way CSS drop-shadow does.
    static const std::vector<PaintDropShadow> none;
    return none;
}

bool JiltvPaint::animated() const
{
    if (this->hasTracks_ || (this->texture_ && !this->loops_.empty()))
    {
        return true;
    }
    return std::ranges::any_of(this->loops_, [](const auto &it) {
        return it.second->animated();
    });
}

bool JiltvPaint::shadowsEnabled() const
{
    return getSettings()->jiltvPaintShadows;
}

qreal JiltvPaint::overflow() const
{
    if (!this->shadowsEnabled())
    {
        return 0;
    }
    // A blur fades out about three standard deviations (three times its value)
    // from the shape. The room is in CSS pixels, but the chat zooms the
    // paint, so it is doubled to leave the glow whole up to a zoom of
    // about 1.3 and nearly whole above.
    constexpr double zoomRoom = 2;
    // The shadows are chained, so their offsets add up and their blurs
    // combine like Gaussians do (variances add).
    double offsets = 0;
    double variance = 0;
    for (const auto &s : this->doc_.shadows)
    {
        offsets += std::max(std::abs(s.x), std::abs(s.y));
        variance += s.blur * s.blur;
    }
    const double stroke = this->doc_.stroke ? this->doc_.stroke->x / 2 : 0;
    return (stroke + offsets + (3 * std::sqrt(variance))) * zoomRoom;
}

QString JiltvPaint::sourceName() const
{
    return QStringLiteral("JilTV");
}

QColor JiltvPaint::fallbackColor() const
{
    return toQColor(this->doc_.fallback);
}

}  // namespace chatterino
