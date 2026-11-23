#include "widgets/helper/CanvasChatSurface.hpp"

#include <QCanvasImage>
#include <QCanvasLinearGradient>
#include <QCanvasPainter>
#include <QCanvasRadialGradient>
#include <QHideEvent>
#include <QLinearGradient>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegion>
#include <QTextItem>

#include <cmath>
#include <list>
#include <unordered_map>
#include <vector>

namespace chatterino {
namespace {
struct TextureCache {
    struct Entry {
        QCanvasImage image;
        qsizetype bytes;
        std::list<qint64>::iterator age;
        uint64_t lastFrame;
    };
    QCanvasPainter *canvas = nullptr;
    std::unordered_map<qint64, Entry> entries;
    std::list<qint64> ages;
    qsizetype bytes = 0;
    uint64_t frame = 0;
    // Every frame of a running paint loop stays resident, so a loop that
    // went round once never uploads again; message buffers fit beside them.
    static constexpr qsizetype budget = qsizetype{64} * 1024 * 1024;
    static constexpr size_t maxEntries = 4096;

    void clear()
    {
        if (canvas)
            for (auto &[key, entry] : entries)
                canvas->removeImage(entry.image);
        entries.clear();
        ages.clear();
        bytes = 0;
    }
    void begin(QCanvasPainter *p)
    {
        canvas = p;
        ++frame;
        // Evict before recording a frame, never invalidate a texture used by
        // an earlier draw in the same frame. GPU resource deletion is deferred by Qt.
        while (!ages.empty() && (bytes > budget || entries.size() > maxEntries))
        {
            auto it = entries.find(ages.back());
            // Keep the active working set even on HiDPI screens; a smaller
            // budget must not upload every visible message again each frame.
            if (it->second.lastFrame + 1 >= frame)
                break;
            canvas->removeImage(it->second.image);
            bytes -= it->second.bytes;
            ages.pop_back();
            entries.erase(it);
        }
    }
    template <typename Load>
    QCanvasImage get(qint64 key, Load load)
    {
        auto found = entries.find(key);
        if (found != entries.end())
        {
            found->second.lastFrame = frame;
            ages.splice(ages.begin(), ages, found->second.age);
            return found->second.image;
        }
        QImage pixels = load();
        if (pixels.isNull())
            return {};
        const auto cost = pixels.sizeInBytes();
        auto image = canvas->addImage(pixels);
        if (image.isNull())
            return {};
        ages.push_front(key);
        entries.emplace(key, Entry{image, cost, ages.begin(), frame});
        bytes += cost;
        return image;
    }
};

class CanvasEngine final : public QPaintEngine
{
public:
    CanvasEngine(QCanvasPainter *p, TextureCache &cache)
        : QPaintEngine(PrimitiveTransform | PixmapTransform | AlphaBlend |
                       ConstantOpacity | Antialiasing | PainterPaths |
                       BrushStroke)
        , canvas_(p)
        , cache_(cache)
    {
    }
    bool begin(QPaintDevice *) override
    {
        setActive(true);
        return true;
    }
    bool end() override
    {
        setActive(false);
        return true;
    }
    Type type() const override
    {
        return User;
    }
    void updateState(const QPaintEngineState &changed) override
    {
        if (changed.state() &
            (QPaintEngine::DirtyClipPath | QPaintEngine::DirtyClipRegion |
             QPaintEngine::DirtyClipEnabled))
        {
            this->clipDirty_ = true;
        }
    }

    template <typename Draw>
    void clipped(Draw draw)
    {
        // The clip in device pixels, worked out once per change of the clip
        // instead of once per draw: resolving QPainter's clip into a region
        // is costly, and draws come by the hundred per frame.
        if (this->clipDirty_)
        {
            this->clipDirty_ = false;
            this->clipRects_.clear();
            this->clipping_ = painter()->hasClipping();
            if (this->clipping_)
            {
                // Qt resolves intersected paths/regions in logical
                // coordinates; the engine transform maps them to the
                // physical pixels of the retained canvas (it includes the
                // device pixel ratio).
                const auto toDevice = state->transform();
                for (const auto &rect : painter()->clipRegion())
                {
                    // Whole pixels, outward: exactly what prePaint
                    // cleared. A scissor only covers whole pixels, so an
                    // edge between two pixels (fractional scaling) would
                    // leave a half-cleared, undrawn line around the area.
                    this->clipRects_.emplace_back(
                        toDevice.mapRect(QRectF(rect)).toAlignedRect());
                }
            }
        }

        canvas_->save();
        canvas_->setGlobalAlpha(state->opacity());
        using Op = QCanvasPainter::CompositeOperation;
        const auto composition = state->compositionMode();
        canvas_->setGlobalCompositeOperation(
            composition == QPainter::CompositionMode_SourceAtop ? Op::SourceAtop
            : composition == QPainter::CompositionMode_DestinationOut
                ? Op::DestinationOut
                : Op::SourceOver);
        const auto transform = state->transform();
        if (this->clipping_)
        {
            // Disjoint dirty rectangles stay apart rather than clipping to
            // their union. Set without a transform, the clip is a plain GPU
            // scissor instead of a per-fragment test.
            for (const auto &rect : this->clipRects_)
            {
                canvas_->resetTransform();
                canvas_->setClipRect(rect);
                canvas_->setTransform(transform);
                draw();
            }
        }
        else
        {
            canvas_->resetClipping();
            canvas_->setTransform(transform);
            draw();
        }
        canvas_->restore();
    }

    /// Fills with `brush` on the GPU: plain colors and linear and radial
    /// gradients as they are, anything else in its first color.
    void setFill(const QBrush &brush)
    {
        const auto *gradient = brush.gradient();
        if (gradient == nullptr ||
            gradient->coordinateMode() != QGradient::LogicalMode ||
            gradient->spread() != QGradient::PadSpread ||
            gradient->stops().isEmpty())
        {
            canvas_->setFillStyle(
                gradient != nullptr && !gradient->stops().isEmpty()
                    ? gradient->stops().constFirst().second
                    : brush.color());
            return;
        }
        // The canvas has no brush transform: a moved or scaled brush moves
        // the gradient's own points; anything else keeps its first color.
        const auto transform = brush.transform();
        if (transform.type() > QTransform::TxScale)
        {
            canvas_->setFillStyle(gradient->stops().constFirst().second);
            return;
        }
        QCanvasGradientStops stops;
        stops.reserve(gradient->stops().size());
        for (const auto &[position, color] : gradient->stops())
        {
            stops.append({static_cast<float>(position), color});
        }
        if (gradient->type() == QGradient::LinearGradient)
        {
            const auto *linear = static_cast<const QLinearGradient *>(gradient);
            QCanvasLinearGradient fill(transform.map(linear->start()),
                                       transform.map(linear->finalStop()));
            fill.setStops(stops);
            canvas_->setFillStyle(fill);
        }
        else if (gradient->type() == QGradient::RadialGradient &&
                 qFuzzyCompare(std::abs(transform.m11()),
                               std::abs(transform.m22())))
        {
            const auto *radial = static_cast<const QRadialGradient *>(gradient);
            const auto scale = std::abs(transform.m11());
            QCanvasRadialGradient fill(transform.map(radial->center()),
                                       radial->radius() * scale,
                                       radial->focalRadius() * scale);
            fill.setStops(stops);
            canvas_->setFillStyle(fill);
        }
        else
        {
            canvas_->setFillStyle(stops.constFirst().color);
        }
    }

    /// Adds `path` as QPainter would fill it: with its own subpath
    /// directions (the canvas would otherwise force every subpath solid)
    /// and its fill rule, which fill() then uses.
    QCanvasPainter::FillRule addPath(const QPainterPath &path)
    {
        canvas_->setWindingEnforce(false);
        canvas_->addPath(path);
        return path.fillRule() == Qt::OddEvenFill
                   ? QCanvasPainter::FillRule::EvenOdd
                   : QCanvasPainter::FillRule::NonZero;
    }
    void drawPixmap(const QRectF &destination, const QPixmap &pixmap,
                    const QRectF &source) override
    {
        auto image = cache_.get(pixmap.cacheKey(), [&] {
            return pixmap.toImage();
        });
        if (!image.isNull())
            clipped([&] {
                canvas_->drawImage(image, source, destination);
            });
    }
    void drawImage(const QRectF &destination, const QImage &pixels,
                   const QRectF &source, Qt::ImageConversionFlags) override
    {
        auto image = cache_.get(pixels.cacheKey(), [&] {
            return pixels;
        });
        if (!image.isNull())
            clipped([&] {
                canvas_->drawImage(image, source, destination);
            });
    }
    void drawPath(const QPainterPath &path) override
    {
        const auto pen = state->pen();
        const auto brush = state->brush();
        clipped([&] {
            canvas_->beginPath();
            const auto rule = this->addPath(path);
            if (brush.style() != Qt::NoBrush)
            {
                this->setFill(brush);
                canvas_->fill(rule);
            }
            if (pen.style() != Qt::NoPen)
            {
                canvas_->setStrokeStyle(pen.color());
                canvas_->setLineWidth(pen.widthF() > 0 ? pen.widthF() : 1);
                canvas_->setLineCap(pen.capStyle() == Qt::RoundCap
                                        ? QCanvasPainter::LineCap::Round
                                    : pen.capStyle() == Qt::SquareCap
                                        ? QCanvasPainter::LineCap::Square
                                        : QCanvasPainter::LineCap::Butt);
                canvas_->setLineJoin(pen.joinStyle() == Qt::RoundJoin
                                         ? QCanvasPainter::LineJoin::Round
                                     : pen.joinStyle() == Qt::BevelJoin
                                         ? QCanvasPainter::LineJoin::Bevel
                                         : QCanvasPainter::LineJoin::Miter);
                canvas_->stroke();
            }
        });
    }
    void drawTextItem(const QPointF &position, const QTextItem &item) override
    {
        clipped([&] {
            canvas_->setFont(item.font());
            canvas_->setFillStyle(state->pen().color());
            canvas_->fillText(item.text(), position);
        });
    }

private:
    QCanvasPainter *canvas_;
    TextureCache &cache_;
    bool clipDirty_ = true;
    bool clipping_ = false;
    std::vector<QRectF> clipRects_;
};

class CanvasDevice final : public QPaintDevice
{
public:
    CanvasDevice(QWidget *widget, QCanvasPainter *p, TextureCache &cache)
        : widget_(widget)
        , engine_(p, cache)
    {
    }
    QPaintEngine *paintEngine() const override
    {
        return const_cast<CanvasEngine *>(&engine_);
    }

protected:
    int metric(PaintDeviceMetric m) const override
    {
        switch (m)
        {
            case PdmWidth:
                return widget_->width();
            case PdmHeight:
                return widget_->height();
            case PdmWidthMM:
                return widget_->widthMM();
            case PdmHeightMM:
                return widget_->heightMM();
            case PdmDpiX:
                return widget_->logicalDpiX();
            case PdmDpiY:
                return widget_->logicalDpiY();
            case PdmPhysicalDpiX:
                return widget_->physicalDpiX();
            case PdmPhysicalDpiY:
                return widget_->physicalDpiY();
            case PdmDepth:
                return 32;
            case PdmNumColors:
                return 0;
            case PdmDevicePixelRatio:
                return qRound(widget_->devicePixelRatioF());
            case PdmDevicePixelRatioScaled:
                return qRound(widget_->devicePixelRatioF() *
                              devicePixelRatioFScale());
            case PdmDevicePixelRatioF_EncodedA:
            case PdmDevicePixelRatioF_EncodedB:
                return encodeMetricF(m, widget_->devicePixelRatioF());
        }
        return 0;
    }

private:
    QWidget *widget_;
    CanvasEngine engine_;
};
}  // namespace
struct CanvasChatSurface::Private {
    std::function<void(QPainter &, const QRegion &)> draw;
    TextureCache textures;
    bool needsInitialize = false;
    QCanvasOffscreenCanvas retained;
    QCanvasImage retainedImage;
    QSize pixelSize;
    QRegion dirty;
    bool regionRequested = false;
};
CanvasChatSurface::CanvasChatSurface(QWidget *parent,
                                     std::function<void(QPainter &)> draw)
    : CanvasChatSurface(parent,
                        [draw = std::move(draw)](QPainter &p, const QRegion &) {
                            draw(p);
                        })
{
}
CanvasChatSurface::CanvasChatSurface(
    QWidget *parent, std::function<void(QPainter &, const QRegion &)> draw)
    : QCanvasPainterWidget(parent)
    , d_(std::make_unique<Private>())
{
    d_->draw = std::move(draw);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setFillColor(Qt::transparent);
    // A surface owns its texture IDs and renderer lifecycle. In particular,
    // hiding or moving another chat must not reset this surface's resources.
    setSharedPainter(false);
#ifdef Q_OS_WIN
    // Each surface now owns its renderer rather than sharing mutable frame
    // resources. Prefer D3D11's smaller host-memory footprint; retain OpenGL
    // as a per-launch workaround for drivers with Direct3D issues.
    setApi(qEnvironmentVariable("JILCHAT_CANVAS_BACKEND") ==
                   QStringLiteral("opengl")
               ? QRhiWidget::Api::OpenGL
               : QRhiWidget::Api::Direct3D11);
#endif
    setGeometry(parent->rect());
    setObjectName(QStringLiteral("canvasChatSurface"));
}
CanvasChatSurface::~CanvasChatSurface()
{
    if (d_->textures.canvas)
        QCanvasPainterWidget::releaseResources();
}
void CanvasChatSurface::render(QRhiCommandBuffer *commands)
{
    if (d_->needsInitialize)
    {
        QCanvasPainterWidget::initialize(commands);
        d_->needsInitialize = false;
    }
    QCanvasPainterWidget::render(commands);
}

void CanvasChatSurface::paint(QCanvasPainter *canvas)
{
    if (!d_->retainedImage.isNull())
        canvas->drawImage(d_->retainedImage, QRectF(rect()));
}
void CanvasChatSurface::repaintRegion(const QRegion &region)
{
    d_->dirty |= region;
    d_->regionRequested = true;
    if (isVisible())
        update();
}
void CanvasChatSurface::prePaint(QCanvasPainter *canvas)
{
    const QSize pixels = (QSizeF(size()) * devicePixelRatioF()).toSize();
    bool fresh = d_->retained.isNull() || d_->pixelSize != pixels;
    if (fresh)
    {
        if (!d_->retainedImage.isNull())
            canvas->removeImage(d_->retainedImage);
        if (!d_->retained.isNull())
            canvas->destroyCanvas(d_->retained);
        d_->retained = canvas->createCanvas(
            pixels, 1, QCanvasOffscreenCanvas::Flag::PreserveContents);
        d_->retainedImage = canvas->addImage(d_->retained);
        d_->pixelSize = pixels;
    }
    const QRegion dirty = fresh || !d_->regionRequested
                              ? QRegion(rect())
                              : d_->dirty.intersected(rect());
    d_->dirty = {};
    d_->regionRequested = false;
    if (dirty.isEmpty() || d_->retained.isNull())
        return;
    d_->textures.begin(canvas);
    beginCanvasPainting(d_->retained);
    // Cleared on whole pixels, the same ones the draws are clipped to (see
    // CanvasEngine::clipped).
    const auto dpr = devicePixelRatioF();
    for (const auto &area : dirty)
        canvas->clearRect(QRectF(QRectF(area.x() * dpr, area.y() * dpr,
                                        area.width() * dpr,
                                        area.height() * dpr)
                                     .toAlignedRect()));
    {
        CanvasDevice device(this, canvas, d_->textures);
        QPainter painter(&device);
        painter.setClipRegion(dirty);
        d_->draw(painter, dirty);
    }
    endCanvasPainting();
}
void CanvasChatSurface::graphicsResourcesInvalidated()
{
    if (d_->textures.canvas)
    {
        if (!d_->retainedImage.isNull())
            d_->textures.canvas->removeImage(d_->retainedImage);
        if (!d_->retained.isNull())
            d_->textures.canvas->destroyCanvas(d_->retained);
    }
    d_->retainedImage = {};
    d_->retained = {};
    d_->dirty = {};
    d_->textures.clear();
    d_->textures.canvas = nullptr;
}
void CanvasChatSurface::hideEvent(QHideEvent *event)
{
    if (d_->textures.canvas)
    {
        // Free the per-tab renderer/pipeline resources, not just image handles.
        // Recreate only after Qt has made the render context current again.
        QCanvasPainterWidget::releaseResources();
        d_->needsInitialize = true;
    }
    QCanvasPainterWidget::hideEvent(event);
}
}  // namespace chatterino
