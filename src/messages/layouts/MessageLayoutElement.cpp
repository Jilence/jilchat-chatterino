// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayoutElement.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvUsernameEffects.hpp"
#include "providers/jilchat/JilChatVoice.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "util/DebugCount.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QDebug>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsPixmapItem>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>
#include <QTransform>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>

namespace {

QRectF snapRectToDevicePixels(const QRectF &rect, const QPainter &painter)
{
    const auto dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const auto snap = [dpr](qreal value) {
        return std::round(value * dpr) / dpr;
    };

    return QRectF(snap(rect.x()), snap(rect.y()),
                  std::max(1.0 / dpr, snap(rect.width())),
                  std::max(1.0 / dpr, snap(rect.height())));
}

const QChar RTL_EMBED(0x202B);

void alignRectBottomCenter(QRectF &rect, const QRectF &reference)
{
    QPointF newCenter(reference.center().x(),
                      reference.bottom() - (rect.height() / 2.0));
    rect.moveCenter(newCenter);
}

void drawPixmapWithOptionalSmoothing(
    QPainter &painter, const QRectF &target, const QPixmap &pixmap,
    chatterino::FlagsEnum<chatterino::MessageElementFlag> flags)
{
    const bool smooth =
        flags.has(chatterino::MessageElementFlag::BadgeMoltorino) ||
        flags.has(chatterino::MessageElementFlag::BadgeBluzyrino);
    const bool wasSmooth =
        painter.testRenderHint(QPainter::SmoothPixmapTransform);

    if (smooth && !wasSmooth)
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }

    painter.drawPixmap(snapRectToDevicePixels(target, painter), pixmap,
                       QRectF());

    if (smooth && !wasSmooth)
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }
}

}  // namespace

namespace chatterino {

const QRectF &MessageLayoutElement::getRect() const
{
    return this->rect_;
}

MessageLayoutElement::MessageLayoutElement(MessageElement &creator, QSizeF size)
    : rect_(QPointF{}, size)
    , creator_(creator)
{
    DebugCount::increase(DebugObject::MessageLayoutElement);
}

MessageLayoutElement::~MessageLayoutElement()
{
    DebugCount::decrease(DebugObject::MessageLayoutElement);
}

MessageElement &MessageLayoutElement::getCreator() const
{
    return this->creator_;
}

void MessageLayoutElement::setPosition(QPointF point)
{
    this->rect_.moveTopLeft(point);
}

bool MessageLayoutElement::hasTrailingSpace() const
{
    return this->trailingSpace;
}

size_t MessageLayoutElement::getLine() const
{
    return this->line_;
}

void MessageLayoutElement::setLine(size_t line)
{
    this->line_ = line;
}

MessageLayoutElement *MessageLayoutElement::setTrailingSpace(bool value)
{
    this->trailingSpace = value;

    return this;
}

MessageLayoutElement *MessageLayoutElement::setLink(const Link &link)
{
    this->link_ = link;
    return this;
}

MessageLayoutElement *MessageLayoutElement::setText(const QString &_text)
{
    this->text_ = _text;
    return this;
}

Link MessageLayoutElement::getLink() const
{
    if (this->link_)
    {
        return *this->link_;
    }
    return this->creator_.getLink();
}

const QString &MessageLayoutElement::getText() const
{
    return this->text_;
}

FlagsEnum<MessageElementFlag> MessageLayoutElement::getFlags() const
{
    return this->creator_.getFlags();
}

int MessageLayoutElement::getWordId() const
{
    return this->wordId_;
}

void MessageLayoutElement::setWordId(int wordId)
{
    this->wordId_ = wordId;
}

//
// IMAGE
//

ImageLayoutElement::ImageLayoutElement(MessageElement &creator, ImagePtr image,
                                       QSizeF size)
    : MessageLayoutElement(creator, size)
    , image_(std::move(image))
{
    this->trailingSpace = creator.hasTrailingSpace();
}

void ImageLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                             uint32_t to) const
{
    const auto *emoteElement =
        dynamic_cast<EmoteElement *>(&this->getCreator());
    if (emoteElement)
    {
        str += emoteElement->getEmote()->getCopyString();
        str = TwitchEmotes::cleanUpEmoteCode(str);
        if (this->hasTrailingSpace() && to >= 2)
        {
            str += ' ';
        }
    }
    else if (const auto *imageElement =
                 dynamic_cast<ScalingImageElement *>(&this->getCreator()))
    {
        str += imageElement->copyText();
        if (!imageElement->copyText().isEmpty() && this->hasTrailingSpace() &&
            to >= 2)
        {
            str += ' ';
        }
    }
}

size_t ImageLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void ImageLayoutElement::paint(QPainter &painter,
                               const MessageColors & /*messageColors*/)
{
    if (this->image_ == nullptr)
    {
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (pixmap && !this->image_->animated())
    {
        drawPixmapWithOptionalSmoothing(painter, QRectF(this->getRect()),
                                        *pixmap, this->getFlags());
    }
}

bool ImageLayoutElement::paintAnimated(QPainter &painter, qreal yOffset)
{
    if (this->image_ == nullptr)
    {
        return false;
    }

    if (this->image_->animated())
    {
        if (auto pixmap = this->image_->pixmapOrLoad())
        {
            auto rect = this->getRect();
            rect.moveTop(rect.y() + yOffset);
            drawPixmapWithOptionalSmoothing(painter, QRectF(rect), *pixmap,
                                            this->getFlags());
            return true;
        }
    }
    return false;
}

int ImageLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() >= this->getRect().center().x())
    {
        return static_cast<int>(this->getSelectionIndexCount());
    }

    return 0;
}

qreal ImageLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

//
// MODIFIER IMAGE
//

ModifierImageLayoutElement::ModifierImageLayoutElement(MessageElement &creator,
                                                       EmotePtr modifier,
                                                       ImagePtr image,
                                                       QSizeF size)
    : ImageLayoutElement(creator, std::move(image), size)
    , modifier_(std::move(modifier))
{
}

const EmotePtr &ModifierImageLayoutElement::modifier() const
{
    return this->modifier_;
}

//
// LAYERED IMAGE
//

namespace {

/// The room an emote takes with the emote effects `flags`: the wide ones
/// make it wider, the others stay within what it had.
QSizeF modifierLayoutSize(QSizeF contentSize, uint32_t flags)
{
    using namespace chatterino::emote_modifiers;

    if ((flags & BTTV_WIDE) != 0)
    {
        return {contentSize.height() * 2.0, contentSize.height()};
    }
    if ((flags & WIDE) != 0)
    {
        return {contentSize.width() * 2.0, contentSize.height()};
    }
    return contentSize;
}

/// The step of the color cycle that the rainbow and party effects are in.
int colorPhase(uint32_t flags, unsigned long time)
{
    using namespace chatterino::emote_modifiers;

    if ((flags & (RAINBOW | PARTY)) == 0)
    {
        return 0;
    }
    constexpr unsigned long PHASES = 12;
    const auto duration = (flags & PARTY) != 0 ? 1500UL : 2000UL;
    return static_cast<int>((time % duration) * PHASES / duration);
}

constexpr int MODIFIER_FILTER_CACHE_KIB = 8 * 1024;

struct ModifierFilterKey {
    qint64 pixmapCacheKey{};
    uint32_t flags{};
    int phase{};

    bool operator==(const ModifierFilterKey &) const = default;
};

size_t qHash(const ModifierFilterKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.pixmapCacheKey, key.flags, key.phase);
}

struct ModifierFilterFrame {
    QPixmap pixmap;
    std::weak_ptr<chatterino::Image> source;
};

QCache<ModifierFilterKey, ModifierFilterFrame> *modifierFilterCache = nullptr;
QSet<ModifierFilterKey> *pendingModifierFilters = nullptr;
QThreadPool *modifierFilterPool = nullptr;
std::atomic_bool modifierFiltersShuttingDown{false};

void clearModifierFilterCache()
{
    modifierFiltersShuttingDown.store(true, std::memory_order_release);
    if (modifierFilterPool != nullptr)
    {
        modifierFilterPool->clear();
        modifierFilterPool->waitForDone();
        delete modifierFilterPool;
        modifierFilterPool = nullptr;
    }
    delete pendingModifierFilters;
    pendingModifierFilters = nullptr;
    delete modifierFilterCache;
    modifierFilterCache = nullptr;
}

QCache<ModifierFilterKey, ModifierFilterFrame> &getModifierFilterCache()
{
    if (modifierFilterCache == nullptr)
    {
        modifierFilterCache =
            new QCache<ModifierFilterKey, ModifierFilterFrame>(
                MODIFIER_FILTER_CACHE_KIB);
        pendingModifierFilters = new QSet<ModifierFilterKey>;
        modifierFilterPool = new QThreadPool;

        modifierFilterPool->setMaxThreadCount(1);
        modifierFilterPool->setExpiryTimeout(10'000);
        modifierFiltersShuttingDown.store(false, std::memory_order_release);

        qAddPostRoutine(clearModifierFilterCache);
    }
    return *modifierFilterCache;
}

int clampColor(qreal value)
{
    return std::clamp(static_cast<int>(std::lround(value)), 0, 255);
}

void applySepia(qreal amount, int &red, int &green, int &blue)
{
    const auto sepiaRed = 0.393 * red + 0.769 * green + 0.189 * blue;
    const auto sepiaGreen = 0.349 * red + 0.686 * green + 0.168 * blue;
    const auto sepiaBlue = 0.272 * red + 0.534 * green + 0.131 * blue;
    red = clampColor(red + (sepiaRed - red) * amount);
    green = clampColor(green + (sepiaGreen - green) * amount);
    blue = clampColor(blue + (sepiaBlue - blue) * amount);
}

void applySaturation(qreal amount, int &red, int &green, int &blue)
{
    const auto luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    red = clampColor(luminance + (red - luminance) * amount);
    green = clampColor(luminance + (green - luminance) * amount);
    blue = clampColor(luminance + (blue - luminance) * amount);
}

void applyContrast(qreal amount, int &red, int &green, int &blue)
{
    const auto adjust = [amount](int channel) {
        return clampColor((channel - 127.5) * amount + 127.5);
    };
    red = adjust(red);
    green = adjust(green);
    blue = adjust(blue);
}

QImage filterModifierImage(QImage image, uint32_t filterFlags, int phase)
{
    using namespace chatterino::emote_modifiers;
    image = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            const auto pixel = line[x];
            const auto alpha = qAlpha(pixel);
            if (alpha == 0)
            {
                continue;
            }

            int red = qRed(pixel);
            int green = qGreen(pixel);
            int blue = qBlue(pixel);
            if ((filterFlags & CURSED) != 0)
            {
                const auto gray =
                    clampColor(0.2126 * red + 0.7152 * green + 0.0722 * blue);
                red = green = blue = clampColor(gray * 0.7);
                applyContrast(2.5, red, green, blue);
            }
            if ((filterFlags & HYPER_RED) != 0)
            {
                red = clampColor(red * 0.2);
                green = clampColor(green * 0.2);
                blue = clampColor(blue * 0.2);
                applySepia(1.0, red, green, blue);
                red = clampColor(red * 2.2);
                green = clampColor(green * 2.2);
                blue = clampColor(blue * 2.2);
                applyContrast(3.0, red, green, blue);
                applySaturation(8.0, red, green, blue);
            }
            if ((filterFlags & (RAINBOW | PARTY)) != 0)
            {
                if ((filterFlags & PARTY) != 0)
                {
                    applySepia(0.5, red, green, blue);
                    applySaturation(2.5, red, green, blue);
                }
                QColor color(red, green, blue);
                int hue = 0;
                int saturation = 0;
                int value = 0;
                color.getHsv(&hue, &saturation, &value);
                hue = (std::max(0, hue) + phase * 30) % 360;
                color.setHsv(hue, saturation, value);
                red = color.red();
                green = color.green();
                blue = color.blue();
            }
            line[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

void insertModifierFilterResult(const ModifierFilterKey &key, QImage image,
                                qreal devicePixelRatio,
                                const ModifierFilterKey &fallbackKey,
                                const std::weak_ptr<chatterino::Image> &source)
{
    if (modifierFiltersShuttingDown.load(std::memory_order_acquire) ||
        modifierFilterCache == nullptr || pendingModifierFilters == nullptr)
    {
        return;
    }

    pendingModifierFilters->remove(key);
    auto result = QPixmap::fromImage(std::move(image));
    result.setDevicePixelRatio(devicePixelRatio);
    const auto costKiB = std::max<qsizetype>(
        1,
        (static_cast<qsizetype>(result.width()) * result.height() * 4 + 1023) /
            1024);
    if (costKiB <= MODIFIER_FILTER_CACHE_KIB)
    {
        modifierFilterCache->insert(key,
                                    new ModifierFilterFrame{result, source},
                                    static_cast<int>(costKiB));
        if (!source.expired())
        {
            modifierFilterCache->insert(
                fallbackKey, new ModifierFilterFrame{std::move(result), source},
                static_cast<int>(costKiB));
        }
    }
}

QPixmap filteredModifierPixmap(const QPixmap &source, uint32_t flags, int phase,
                               const chatterino::ImagePtr &sourceImage)
{
    using namespace chatterino::emote_modifiers;
    const auto filterFlags = flags & (RAINBOW | PARTY | HYPER_RED | CURSED);
    if (filterFlags == 0 || source.isNull())
    {
        return source;
    }

    const ModifierFilterKey key{
        .pixmapCacheKey = source.cacheKey(),
        .flags = filterFlags,
        .phase = phase,
    };
    auto &cache = getModifierFilterCache();
    if (const auto *cached = cache.object(key))
    {
        return cached->pixmap;
    }

    const bool deferFiltering =
        sourceImage->animated() || (filterFlags & (RAINBOW | PARTY)) != 0;
    const ModifierFilterKey fallbackKey{
        .pixmapCacheKey =
            static_cast<qint64>(reinterpret_cast<quintptr>(sourceImage.get())),
        .flags = filterFlags,
        .phase = -1,
    };
    if (deferFiltering)
    {
        QPixmap fallback;
        if (const auto *last = cache.object(fallbackKey);
            last && last->source.lock() == sourceImage)
        {
            fallback = last->pixmap;
        }
        if (fallback.isNull())
        {
            constexpr int PREVIEW_DIMENSION = 128;
            auto preview = source;
            if (std::max(source.width(), source.height()) > PREVIEW_DIMENSION)
            {
                preview = source.scaled(PREVIEW_DIMENSION, PREVIEW_DIMENSION,
                                        Qt::KeepAspectRatio);
            }
            fallback = QPixmap::fromImage(
                filterModifierImage(preview.toImage(), filterFlags, phase));
            fallback.setDevicePixelRatio(source.devicePixelRatio());
            const auto costKiB = std::max(
                1, (fallback.width() * fallback.height() * 4 + 1023) / 1024);
            cache.insert(fallbackKey,
                         new ModifierFilterFrame{fallback, sourceImage},
                         costKiB);
            if (preview.size() == source.size())
            {
                cache.insert(key,
                             new ModifierFilterFrame{fallback, sourceImage},
                             costKiB);
                return fallback;
            }
        }
        if (pendingModifierFilters == nullptr ||
            modifierFilterPool == nullptr ||
            pendingModifierFilters->contains(key) ||
            pendingModifierFilters->size() >= 16 ||
            modifierFilterPool->activeThreadCount() >=
                modifierFilterPool->maxThreadCount())
        {
            return fallback;
        }

        pendingModifierFilters->insert(key);
        const auto dpr = source.devicePixelRatio();
        auto image = source.toImage();
        std::weak_ptr<chatterino::Image> weakSource = sourceImage;
        auto *context = QCoreApplication::instance();
        const bool started =
            context != nullptr &&
            modifierFilterPool->tryStart([key, image = std::move(image),
                                          filterFlags, phase, dpr, fallbackKey,
                                          weakSource, context]() mutable {
                auto filtered =
                    filterModifierImage(std::move(image), filterFlags, phase);
                if (modifierFiltersShuttingDown.load(std::memory_order_acquire))
                {
                    return;
                }
                QMetaObject::invokeMethod(
                    context,
                    [key, filtered = std::move(filtered), dpr, fallbackKey,
                     weakSource]() mutable {
                        insertModifierFilterResult(key, std::move(filtered),
                                                   dpr, fallbackKey,
                                                   weakSource);
                    },
                    Qt::QueuedConnection);
            });
        if (!started)
        {
            pendingModifierFilters->remove(key);
        }
        return fallback;
    }

    auto result = QPixmap::fromImage(
        filterModifierImage(source.toImage(), filterFlags, phase));
    result.setDevicePixelRatio(source.devicePixelRatio());
    const auto costKiB = std::max<qsizetype>(
        1,
        (static_cast<qsizetype>(result.width()) * result.height() * 4 + 1023) /
            1024);
    if (costKiB <= MODIFIER_FILTER_CACHE_KIB)
    {
        cache.insert(key, new ModifierFilterFrame{result, {}},
                     static_cast<int>(costKiB));
    }
    return result;
}

}  // namespace

LayeredImageLayoutElement::LayeredImageLayoutElement(
    MessageElement &creator, std::vector<ImagePtr> images,
    std::vector<QSizeF> sizes, QSizeF largestSize, uint32_t modifierFlags)
    : MessageLayoutElement(creator,
                           modifierLayoutSize(largestSize, modifierFlags))
    , images_(std::move(images))
    , sizes_(std::move(sizes))
    , contentSize_(largestSize)
    , modifierFlags_(modifierFlags)
{
    assert(this->images_.size() == this->sizes_.size());
    this->trailingSpace = creator.hasTrailingSpace();
}

bool LayeredImageLayoutElement::removesPreviousSpace() const
{
    return (this->modifierFlags_ & emote_modifiers::ZERO_SPACE) != 0;
}

void LayeredImageLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                    uint32_t to) const
{
    const auto *layeredEmoteElement =
        dynamic_cast<LayeredEmoteElement *>(&this->getCreator());
    if (layeredEmoteElement)
    {
        // cleaning is taken care in call
        str += layeredEmoteElement->getCleanCopyString();
        if (this->hasTrailingSpace() && to >= 2)
        {
            str += ' ';
        }
    }
}

size_t LayeredImageLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void LayeredImageLayoutElement::paint(QPainter &painter,
                                      const MessageColors & /*messageColors*/)
{
    if ((this->modifierFlags_ & ~emote_modifiers::ZERO_SPACE) != 0)
    {
        // Something that moves is drawn in paintAnimated, in every frame.
        if (!this->needsAnimatedPaint())
        {
            this->paintModified(painter, 0);
        }
        return;
    }

    auto fullRect = QRectF(this->getRect());

    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        auto &img = this->images_[i];
        if (img == nullptr)
        {
            continue;
        }

        auto pixmap = img->pixmapOrLoad();
        if (img->animated())
        {
            // As soon as we see an animated emote layer, we can stop rendering
            // the static emotes. The paintAnimated function will render any
            // static emotes layered on top of the first seen animated emote.
            return;
        }

        if (pixmap)
        {
            // Matching the web chat behavior, we center the emote within the overall
            // binding box. E.g. small overlay emotes like cvMask will sit in the direct
            // center of even wide emotes.
            auto &size = this->sizes_[i];
            QRectF destRect(0, 0, size.width(), size.height());
            alignRectBottomCenter(destRect, fullRect);

            painter.drawPixmap(snapRectToDevicePixels(destRect, painter),
                               *pixmap, QRectF());
        }
    }
}

bool LayeredImageLayoutElement::needsAnimatedPaint() const
{
    if (getSettings()->animateEmotes &&
        (this->modifierFlags_ & emote_modifiers::ANIMATED) != 0)
    {
        return true;
    }
    return std::ranges::any_of(this->images_, [](const auto &image) {
        return image != nullptr && image->animated();
    });
}

void LayeredImageLayoutElement::paintModified(QPainter &painter, qreal yOffset)
{
    using namespace emote_modifiers;

    auto fullRect = QRectF(this->getRect());
    fullRect.translate(0, yOffset);
    QRectF contentRect(QPointF{}, this->contentSize_);
    contentRect.moveCenter(fullRect.center());

    const bool animate = getSettings()->animateEmotes;
    const auto animatedFlags = animate ? this->modifierFlags_ : 0;
    const auto time =
        animate ? getApp()->getEmotes()->getGIFTimer()->position() : 0UL;
    const auto cycle = [](unsigned long position, unsigned long duration) {
        return static_cast<qreal>(position % duration) /
               static_cast<qreal>(duration);
    };
    constexpr qreal PI = 3.14159265358979323846;
    const auto colorFlags = this->modifierFlags_ & COLORS;
    const auto phase = colorPhase(animatedFlags, time);

    qreal scaleX = 1.0;
    if ((this->modifierFlags_ & BTTV_WIDE) != 0 &&
        this->contentSize_.width() > 0)
    {
        scaleX = this->contentSize_.height() * 2.0 / this->contentSize_.width();
    }
    else if ((this->modifierFlags_ & WIDE) != 0)
    {
        scaleX = 2.0;
    }
    qreal scaleY = 1.0;
    if ((this->modifierFlags_ & FLIP_X) != 0)
    {
        scaleX = -scaleX;
    }
    if ((this->modifierFlags_ & FLIP_Y) != 0)
    {
        scaleY = -scaleY;
    }

    qreal rotation = 0.0;
    if ((this->modifierFlags_ & ROTATE_90) != 0)
    {
        rotation += 90.0;
    }
    if ((this->modifierFlags_ & ROTATE_LEFT) != 0)
    {
        rotation -= 90.0;
    }
    if ((this->modifierFlags_ & ROTATE_RIGHT) != 0)
    {
        rotation += 90.0;
    }
    if ((animatedFlags & SPIN) != 0)
    {
        rotation += cycle(time, 1500) * 360.0;
    }

    QPointF translation;
    if ((animatedFlags & SLIDE) != 0)
    {
        const auto duration = static_cast<unsigned long>(
            std::clamp(contentRect.width() / 32.0 * 500.0, 250.0, 2000.0));
        translation.rx() += std::sin(cycle(time, duration) * PI * 2.0) *
                            contentRect.width() * 0.28;
    }
    if ((animatedFlags & (HYPER_RED | SHAKE)) != 0)
    {
        static constexpr std::array<QPointF, 10> FFZ_SHAKE{
            QPointF{-2, 1}, QPointF{3, -2}, QPointF{-1, -3}, QPointF{2, 2},
            QPointF{-3, 0}, QPointF{1, 3},  QPointF{3, 1},   QPointF{-2, -1},
            QPointF{0, 2},  QPointF{1, -2},
        };
        translation += FFZ_SHAKE.at((time / 10U) % FFZ_SHAKE.size());
    }
    if ((animatedFlags & BTTV_SHAKE) != 0)
    {
        static constexpr std::array<QPointF, 10> BTTV_SHAKE_STEPS{
            QPointF{-1, 0}, QPointF{2, -1},  QPointF{-2, 1}, QPointF{1, 2},
            QPointF{0, -2}, QPointF{-2, -1}, QPointF{2, 1},  QPointF{-1, 2},
            QPointF{1, -1}, QPointF{0, 0},
        };
        translation +=
            BTTV_SHAKE_STEPS.at((time / 50U) % BTTV_SHAKE_STEPS.size());
    }

    qreal opacity = 1.0;
    qreal animatedScaleX = 1.0;
    qreal animatedScaleY = 1.0;
    const bool appears = (animatedFlags & APPEAR) != 0;
    const bool leaves = (animatedFlags & LEAVE) != 0;
    const auto enter = [&](qreal progress) {
        progress = std::clamp(progress, 0.0, 1.0);
        const auto eased = 1.0 - std::pow(1.0 - progress, 3.0);
        opacity = progress;
        animatedScaleX = animatedScaleY =
            eased + std::sin(progress * PI) * 0.04;
        translation.rx() -= (1.0 - eased) * contentRect.width() * 0.65;
    };
    const auto leave = [&](qreal progress) {
        progress = std::clamp(progress, 0.0, 1.0);
        const auto remaining = 1.0 - progress;
        opacity = remaining;
        animatedScaleX = remaining * std::cos(progress * PI);
        animatedScaleY = remaining;
        translation.rx() -= progress * contentRect.width() * 0.65;
    };
    if (appears && leaves)
    {
        const auto progress = cycle(time, 6000);
        if (progress < 0.1 || progress >= 0.925)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress < 0.325)
        {
            enter((progress - 0.1) / 0.225);
        }
        else if (progress >= 0.7)
        {
            leave((progress - 0.7) / 0.225);
        }
    }
    else if (appears)
    {
        const auto progress = cycle(time, 3000);
        if (progress < 0.2)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress < 0.65)
        {
            enter((progress - 0.2) / 0.45);
        }
    }
    else if (leaves)
    {
        const auto progress = cycle(time, 3000);
        if (progress >= 0.85)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress >= 0.4)
        {
            leave((progress - 0.4) / 0.45);
        }
    }
    if ((animatedFlags & JAM) != 0)
    {
        const auto beat = std::sin(cycle(time, 600) * PI * 2.0);
        scaleX *= 1.0 + beat * 0.08;
        scaleY *= 1.0 - beat * 0.12;
        translation +=
            QPointF(beat * 3.0, std::cos(cycle(time, 600) * PI * 2.0) * 4.0);
        rotation += -2.5 + beat * 5.5;
    }
    if ((animatedFlags & BOUNCE) != 0)
    {
        const auto progress = cycle(time, 500);
        const auto squash = std::sin(progress * PI);
        scaleX *= 1.0 + squash * 0.2;
        scaleY *= 1.0 - squash * 0.65;
        if (progress >= 0.25 && progress < 0.75)
        {
            scaleX = -scaleX;
        }
    }
    scaleX *= animatedScaleX;
    scaleY *= animatedScaleY;

    QTransform transform;
    auto origin = contentRect.center();
    if ((animatedFlags & BOUNCE) != 0)
    {
        origin.setY(contentRect.bottom());
    }
    transform.translate(origin.x() + translation.x(),
                        origin.y() + translation.y());
    transform.rotate(rotation);
    transform.scale(scaleX, scaleY);
    transform.translate(-origin.x(), -origin.y());

    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
    painter.setWorldTransform(transform, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        const auto &img = this->images_[i];
        if (img == nullptr)
        {
            continue;
        }
        if (auto pixmap = img->pixmapOrLoad())
        {
            // Like without effects, each layer is centered at the bottom of
            // the box around all of them.
            const auto &size = this->sizes_[i];
            QRectF destRect(0, 0, size.width(), size.height());
            alignRectBottomCenter(destRect, contentRect);
            if (colorFlags != 0)
            {
                painter.drawPixmap(
                    destRect,
                    filteredModifierPixmap(*pixmap, colorFlags, phase, img),
                    QRectF());
            }
            else
            {
                painter.drawPixmap(destRect, *pixmap, QRectF());
            }
        }
    }
    painter.restore();
}

bool LayeredImageLayoutElement::paintAnimated(QPainter &painter, qreal yOffset)
{
    if ((this->modifierFlags_ & ~emote_modifiers::ZERO_SPACE) != 0)
    {
        if (!this->needsAnimatedPaint())
        {
            return false;
        }
        this->paintModified(painter, yOffset);
        return true;
    }

    auto fullRect = QRectF(this->getRect());
    fullRect.moveTop(fullRect.y() + yOffset);
    bool animatedFlag = false;

    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        auto &img = this->images_[i];
        if (img == nullptr)
        {
            continue;
        }

        // If we have a static emote layered on top of an animated emote, we need
        // to render the static emote again after animating anything below it.
        if (img->animated() || animatedFlag)
        {
            if (auto pixmap = img->pixmapOrLoad())
            {
                // Matching the web chat behavior, we center the emote within the overall
                // binding box. E.g. small overlay emotes like cvMask will sit in the direct
                // center of even wide emotes.
                auto &size = this->sizes_[i];
                QRectF destRect(0, 0, size.width(), size.height());
                alignRectBottomCenter(destRect, fullRect);

                painter.drawPixmap(snapRectToDevicePixels(destRect, painter),
                                   *pixmap, QRectF());
                animatedFlag = true;
            }
        }
    }
    return animatedFlag;
}

int LayeredImageLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() >= this->getRect().center().x())
    {
        return static_cast<int>(this->getSelectionIndexCount());
    }

    return 0;
}

qreal LayeredImageLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

//
// IMAGE WITH BACKGROUND
//
ImageWithBackgroundLayoutElement::ImageWithBackgroundLayoutElement(
    MessageElement &creator, ImagePtr image, QSizeF size, QColor color)
    : ImageLayoutElement(creator, std::move(image), size)
    , color_(color)
{
}

void ImageWithBackgroundLayoutElement::paint(
    QPainter &painter, const MessageColors & /*messageColors*/)
{
    if (this->image_ == nullptr)
    {
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (pixmap && !this->image_->animated())
    {
        painter.fillRect(QRectF(this->getRect()), this->color_);

        // fourtf: make it use qreal values
        painter.drawPixmap(QRectF(this->getRect()), *pixmap, QRectF());
    }
}

//
// IMAGE WITH CIRCLE BACKGROUND
//
ImageWithCircleBackgroundLayoutElement::ImageWithCircleBackgroundLayoutElement(
    MessageElement &creator, ImagePtr image, const QSize &imageSize,
    QColor color, int padding)
    : ImageLayoutElement(creator, image,
                         imageSize + QSize(padding, padding) * 2)
    , color_(color)
    , imageSize_(imageSize)
    , padding_(padding)
{
}

void ImageWithCircleBackgroundLayoutElement::paint(
    QPainter &painter, const MessageColors & /*messageColors*/)
{
    if (this->image_ == nullptr)
    {
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (pixmap && !this->image_->animated())
    {
        QRectF boxRect(this->getRect());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QBrush(this->color_, Qt::SolidPattern));
        painter.drawEllipse(boxRect);

        QRectF imgRect;
        imgRect.setTopLeft(boxRect.topLeft());
        imgRect.setSize(this->imageSize_);
        imgRect.translate(this->padding_, this->padding_);

        painter.drawPixmap(imgRect, *pixmap, QRectF());
    }
}

//
// TEXT
//

TextLayoutElement::TextLayoutElement(MessageElement &_creator, QString &_text,
                                     QSizeF size, QColor _color,
                                     FontStyle _style,
                                     MessageColor::Type messageColor,
                                     float _scale, float dpr)
    : MessageLayoutElement(_creator, size)
    , color_(_color)
    , style_(_style)
    , messageColor_(messageColor)
    , scale_(_scale)
    , dpr_(dpr)
{
    this->setText(_text);
}

void TextLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                            uint32_t to) const
{
    str += this->getText().mid(from, to - from);

    if (this->hasTrailingSpace() && to > this->getText().length())
    {
        str += ' ';
    }
}

size_t TextLayoutElement::getSelectionIndexCount() const
{
    return this->getText().length() + (this->trailingSpace ? 1 : 0);
}

void TextLayoutElement::paint(QPainter &painter,
                              const MessageColors & /*messageColors*/)
{
    auto *app = getApp();
    QString text = this->getText();
    if (text.isRightToLeft() || this->reversedNeutral)
    {
        text.prepend(RTL_EMBED);
    }

    auto font = app->getFonts()->getFont(this->style_, this->scale_);

    bool isNametag = this->getLink().type == chatterino::Link::UserInfo ||
                     this->getLink().type == chatterino::Link::UserWhisper;
    bool drawPaint = isNametag && this->messageColor_ != MessageColor::System &&
                     usernamePaintsEnabled();
    if (drawPaint)
    {
        auto paint = usernamePaint(this->getLink().value.toLower(),
                                   this->getCreator().getFlags().has(
                                       MessageElementFlag::KickUsername));
        if (paint)
        {
            if (paint->animated())
            {
                return;
            }

            // Some paints reach beyond the name, e.g. with an outline.
            const auto overflow = paint->overflow();
            auto paintPixmap = paint->getPixmap(
                this->getText(), font, this->color_, this->getRect().size(),
                this->scale_, this->dpr_, false, overflow);

            painter.drawPixmap(
                this->getRect().topLeft() - QPointF(overflow, overflow),
                paintPixmap);
            return;
        }
    }

    painter.setPen(this->color_);
    painter.setFont(font);

    const QFontMetricsF metrics(font);
    if (this->getRect().height() > std::ceil(metrics.height()))
    {
        const auto baseline = this->getRect().bottom() - metrics.descent();
        painter.drawText(QPointF(this->getRect().x(), baseline), text);
    }
    else
    {
        painter.drawText(
            QRectF(this->getRect().x(), this->getRect().y(), 10000, 10000),
            text, QTextOption(Qt::AlignLeft | Qt::AlignTop));
    }
}

bool TextLayoutElement::paintAnimated(QPainter &painter, const qreal yOffset)
{
    if (this->getRect().isEmpty())
    {
        return false;
    }

    const auto font = getApp()->getFonts()->getFont(this->style_, this->scale_);

    const bool isNametag =
        this->getLink().type == chatterino::Link::UserInfo ||
        this->getLink().type == chatterino::Link::UserWhisper;
    const bool drawPaint = isNametag && usernamePaintsEnabled();
    if (!drawPaint)
    {
        return false;
    }
    const auto paint = usernamePaint(
        this->getLink().value.toLower(),
        this->getCreator().getFlags().has(MessageElementFlag::KickUsername));
    if (!paint || !paint->animated())
    {
        return false;
    }

    const auto overflow = paint->overflow();
    const auto paintPixmap = paint->getPixmap(
        this->getText(), font, this->color_, this->getRect().size(),
        this->scale_, this->dpr_, false, overflow);

    auto rect = this->getRect();
    rect.moveTop(rect.y() + yOffset);
    rect.adjust(-overflow, -overflow, overflow, overflow);
    painter.drawPixmap(rect, paintPixmap, QRectF());
    return true;
}

int TextLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() < this->getRect().left())
    {
        return 0;
    }

    auto *app = getApp();

    auto metrics = app->getFonts()->getFontMetrics(this->style_, this->scale_);
    auto x = this->getRect().left();

    for (auto i = 0; i < this->getText().size(); i++)
    {
        auto &&text = this->getText();
        auto width = metrics.horizontalAdvance(this->getText()[i]);

        // accept mouse to be at only 50%+ of character width to increase index
        if (x + (width * 0.5) > abs.x())
        {
            if (text.size() > i + 1 &&
                QChar::isLowSurrogate(static_cast<char32_t>(text[i].unicode())))
            {
                i++;
            }

            return i;
        }

        x += width;
    }

    //    if (this->hasTrailingSpace() && abs.x() < this->getRect().right())
    //    {
    //        return this->getSelectionIndexCount() - 1;
    //    }

    return this->getSelectionIndexCount() - (this->hasTrailingSpace() ? 1 : 0);
}

qreal TextLayoutElement::getXFromIndex(size_t index)
{
    auto *app = getApp();

    auto metrics = app->getFonts()->getFontMetrics(this->style_, this->scale_);

    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index < static_cast<size_t>(this->getText().size()))
    {
        qreal x = 0;
        for (size_t i = 0; i < index; i++)
        {
            x += metrics.horizontalAdvance(
                this->getText()[static_cast<QString::size_type>(i)]);
        }
        return x + this->getRect().left();
    }
    else
    {
        return this->getRect().right();
    }
}

// TEXT ICON
TextIconLayoutElement::TextIconLayoutElement(MessageElement &creator,
                                             const QString &_line1,
                                             const QString &_line2,
                                             float _scale, QSizeF size)
    : MessageLayoutElement(creator, size)
    , scale(_scale)
    , line1(_line1)
    , line2(_line2)
{
}

void TextIconLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                uint32_t to) const
{
}

size_t TextIconLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void TextIconLayoutElement::paint(QPainter &painter,
                                  const MessageColors &messageColors)
{
    auto *app = getApp();

    QFont font = app->getFonts()->getFont(FontStyle::Tiny, this->scale);

    painter.setPen(messageColors.systemText);
    painter.setFont(font);

    QTextOption option;
    option.setAlignment(Qt::AlignHCenter);

    if (this->line2.isEmpty())
    {
        painter.drawText(this->getRect(), this->line1, option);
    }
    else
    {
        painter.drawText(
            QPointF{
                this->getRect().x(),
                this->getRect().y() + (this->getRect().height() / 2),
            },
            this->line1);
        painter.drawText(
            QPointF{
                this->getRect().x(),
                this->getRect().y() + this->getRect().height(),
            },
            this->line2);
    }
}

bool TextIconLayoutElement::paintAnimated(QPainter & /*painter*/,
                                          qreal /*yOffset*/)
{
    return false;
}

int TextIconLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() >= this->getRect().center().x())
    {
        return static_cast<int>(this->getSelectionIndexCount());
    }

    return 0;
}

qreal TextIconLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

VoiceMessageLayoutElement::VoiceMessageLayoutElement(MessageElement &creator,
                                                     QString voiceId,
                                                     QSizeF size, float scale)
    : MessageLayoutElement(creator, size)
    , voiceId_(std::move(voiceId))
    , scale_(scale)
{
    this->trailingSpace = creator.hasTrailingSpace();
}

QRectF VoiceMessageLayoutElement::playButtonRect() const
{
    const auto rect = this->getRect();
    return QRectF(rect.left(), rect.top(), 28 * this->scale_, rect.height());
}

QRectF VoiceMessageLayoutElement::waveformRect() const
{
    const auto rect = this->getRect();
    const qreal barsLeft = rect.left() + 30 * this->scale_;
    const qreal barsRight = rect.right() - 54 * this->scale_;
    return {barsLeft, rect.top(), std::max<qreal>(0, barsRight - barsLeft),
            rect.height()};
}

bool VoiceMessageLayoutElement::isOverPlayButton(QPointF point) const
{
    return this->playButtonRect().contains(point);
}

std::optional<double> VoiceMessageLayoutElement::seekProgressAt(
    QPointF point) const
{
    const auto wave = this->waveformRect();
    if (!wave.contains(point) || wave.width() <= 0)
    {
        return std::nullopt;
    }

    return std::clamp((point.x() - wave.left()) / wave.width(), 0.0, 1.0);
}

void VoiceMessageLayoutElement::addCopyTextToString(QString &str,
                                                    uint32_t /*from*/,
                                                    uint32_t to) const
{
    str += QStringLiteral("https://jil.chat/v/%1").arg(this->voiceId_);
    if (this->hasTrailingSpace() && to >= 2)
    {
        str += ' ';
    }
}

size_t VoiceMessageLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void VoiceMessageLayoutElement::paint(QPainter &painter,
                                      const MessageColors &messageColors)
{
    const auto rect = this->getRect();
    const qreal radius = rect.height() / 2.0;
    const QColor accent(145, 66, 255);
    QColor bg = messageColors.regularText;
    bg.setAlphaF(0.08);
    QColor muted = messageColors.regularText;
    muted.setAlphaF(0.24);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(rect, radius, radius);

    const qreal iconX = rect.left() + 10 * this->scale_;
    const qreal iconY = rect.center().y();
    painter.setBrush(accent);
    if (jilchat::isVoicePlaying(this->voiceId_))
    {
        // Playing: show a pause icon (two bars) in the same filled accent style.
        const qreal barW = 2.5 * this->scale_;
        const qreal barH = 10 * this->scale_;
        const qreal gap = 3 * this->scale_;
        const qreal top = iconY - barH / 2.0;
        painter.drawRoundedRect(QRectF(iconX, top, barW, barH), barW / 2.0,
                                barW / 2.0);
        painter.drawRoundedRect(QRectF(iconX + barW + gap, top, barW, barH),
                                barW / 2.0, barW / 2.0);
    }
    else
    {
        QPainterPath triangle;
        triangle.moveTo(iconX, iconY - 5 * this->scale_);
        triangle.lineTo(iconX, iconY + 5 * this->scale_);
        triangle.lineTo(iconX + 8 * this->scale_, iconY);
        triangle.closeSubpath();
        painter.drawPath(triangle);
    }

    const qreal barWidth = 2 * this->scale_;
    const qreal gap = 2 * this->scale_;
    const auto wave = this->waveformRect();
    const qreal barsLeft = wave.left();
    const qreal barsRight = wave.right();
    const int count = std::max(
        1, static_cast<int>((barsRight - barsLeft) / (barWidth + gap)));
    for (int i = 0; i < count; ++i)
    {
        const qreal height = this->barHeight(i, count) * this->scale_;
        const QRectF bar(barsLeft + i * (barWidth + gap),
                         rect.center().y() - (height / 2.0), barWidth, height);
        painter.setBrush(muted);
        painter.drawRoundedRect(bar, barWidth / 2.0, barWidth / 2.0);
    }

    const auto volumeText =
        QStringLiteral("%1%").arg(jilchat::getVoiceVolume(this->voiceId_));
    auto volumeFont =
        getApp()->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale_);
    if (volumeText.size() >= 4)
    {
        volumeFont.setPointSizeF(volumeFont.pointSizeF() * 0.82);
    }
    painter.setPen(messageColors.systemText);
    painter.setFont(volumeFont);
    painter.drawText(QRectF(rect.right() - 52 * this->scale_, rect.top(),
                            43 * this->scale_, rect.height()),
                     Qt::AlignVCenter | Qt::AlignRight, volumeText);
    painter.restore();
}

bool VoiceMessageLayoutElement::paintAnimated(QPainter &painter, qreal yOffset)
{
    const auto progress = jilchat::getVoiceProgress(this->voiceId_);
    if (progress <= 0.0)
    {
        return true;
    }

    auto rect = this->getRect();
    rect.moveTop(rect.y() + yOffset);
    auto wave = this->waveformRect();
    wave.moveTop(wave.y() + yOffset);
    const qreal barWidth = 2 * this->scale_;
    const qreal gap = 2 * this->scale_;
    const qreal filledRight = wave.left() + wave.width() * progress;
    const int count =
        std::max(1, static_cast<int>(wave.width() / (barWidth + gap)));
    const QColor accent(145, 66, 255);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);

    for (int i = 0; i < count; ++i)
    {
        const qreal x = wave.left() + i * (barWidth + gap);
        if (x > filledRight)
        {
            break;
        }

        const qreal height = this->barHeight(i, count) * this->scale_;
        const QRectF bar(x, rect.center().y() - (height / 2.0), barWidth,
                         height);
        painter.drawRoundedRect(bar, barWidth / 2.0, barWidth / 2.0);
    }

    painter.restore();
    return true;
}

int VoiceMessageLayoutElement::getMouseOverIndex(QPointF /*abs*/) const
{
    return 0;
}

qreal VoiceMessageLayoutElement::getXFromIndex(size_t index)
{
    return index <= 0 ? this->getRect().left() : this->getRect().right();
}

qreal VoiceMessageLayoutElement::barHeight(int index, int /*count*/) const
{
    uint h = uint(index) * 2654435761U;
    for (const auto c : this->voiceId_)
    {
        h = (h * 31U) + uint(c.unicode());
    }
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    const qreal norm = qreal(h & 0xFFFFU) / qreal(0xFFFFU);
    return 4 + norm * 12;
}

ReplyCurveLayoutElement::ReplyCurveLayoutElement(MessageElement &creator,
                                                 qreal width, float thickness,
                                                 float radius,
                                                 float neededMargin)
    : MessageLayoutElement(creator, QSizeF(width, 0))
    , pen_(QColor("#888"), thickness, Qt::SolidLine, Qt::RoundCap)
    , radius_(radius)
    , neededMargin_(neededMargin)
{
}

void ReplyCurveLayoutElement::paint(QPainter &painter,
                                    const MessageColors & /*messageColors*/)
{
    QRectF paintRect(this->getRect());
    QPainterPath path;

    QRectF curveRect = paintRect.marginsRemoved(QMarginsF(
        this->neededMargin_, this->neededMargin_, 0, this->neededMargin_));

    // Make sure that our curveRect can always fit the radius curve
    if (curveRect.height() < this->radius_)
    {
        curveRect.setTop(curveRect.top() -
                         (this->radius_ - curveRect.height()));
    }

    QPointF bStartPoint(curveRect.left(), curveRect.top() + this->radius_);
    QPointF bEndPoint(curveRect.left() + this->radius_, curveRect.top());
    QPointF bControlPoint(curveRect.topLeft());

    // Draw line from bottom left to curve
    path.moveTo(curveRect.bottomLeft());
    path.lineTo(bStartPoint);

    // Draw curve path
    path.quadTo(bControlPoint, bEndPoint);

    // Draw line from curve to top right
    path.lineTo(curveRect.topRight());

    // Render curve
    painter.setPen(this->pen_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawPath(path);
}

bool ReplyCurveLayoutElement::paintAnimated(QPainter & /*painter*/,
                                            qreal /*yOffset*/)
{
    return false;
}

int ReplyCurveLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() >= this->getRect().center().x())
    {
        return static_cast<int>(this->getSelectionIndexCount());
    }

    return 0;
}

qreal ReplyCurveLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }

    return this->getRect().right();
}

void ReplyCurveLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                  uint32_t to) const
{
}

size_t ReplyCurveLayoutElement::getSelectionIndexCount() const
{
    return 1;
}

}  // namespace chatterino
