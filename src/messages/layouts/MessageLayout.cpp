// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/highlights/types/AutomodCaughtHighlight.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace chatterino {

namespace {

constexpr int SCROLLBAR_PADDING = 20;

std::optional<QColor> clientDetectionHighlightColor(
    Message::ClientDetectionStatus status,
    const MessagePreferences &preferences)
{
    if (!preferences.enableClientDetectionHighlight)
    {
        return std::nullopt;
    }

    switch (status)
    {
        case Message::ClientDetectionStatus::Web:
            return preferences.clientDetectionWebColor;
        case Message::ClientDetectionStatus::Android:
            return preferences.clientDetectionAndroidColor;
        case Message::ClientDetectionStatus::IOS:
            return preferences.clientDetectionIosColor;
        case Message::ClientDetectionStatus::MobileNew:
            return preferences.clientDetectionMobileNewColor;
        case Message::ClientDetectionStatus::Unknown:
        case Message::ClientDetectionStatus::Abnormal:
            return std::nullopt;
    }

    return std::nullopt;
}

QColor platformTint(MessagePlatform platform)
{
    constexpr int a = 22;
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            return {145, 70, 255, a};
        case MessagePlatform::YouTube:
            return {255, 0, 0, a};
        case MessagePlatform::Kick:
            return {83, 252, 24, a};
    }
    return Qt::transparent;
}
}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

int MessageLayout::getHeight() const
{
    return static_cast<int>(this->container_.getHeight());
}

int MessageLayout::getFirstLineHeight() const
{
    return this->container_.getFirstLineHeight();
}

int MessageLayout::getWidth() const
{
    return static_cast<int>(this->container_.getWidth());
}

int MessageLayout::getLayoutContentWidth() const
{
    return static_cast<int>(
        std::ceil(this->container_.getLayoutContentWidth()));
}

size_t MessageLayout::getLineCount() const
{
    return this->container_.getLineCount();
}

void MessageLayout::setAutoModReviewSelected(bool selected)
{
    if (this->flags.has(MessageLayoutFlag::AutoModReviewSelected) == selected)
    {
        return;
    }

    this->flags.set(MessageLayoutFlag::AutoModReviewSelected, selected);
    this->flags.set(MessageLayoutFlag::RequiresLayout);
    this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
}

bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    bool layoutRequired = false;

    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;

    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    bool scaleChanged = this->scale_ != ctx.scale ||
                        this->imageScale_ != ctx.imageScale ||
                        this->emoteScale_ != ctx.emoteScale ||
                        this->badgeScale_ != ctx.badgeScale ||
                        this->centerBadges_ != ctx.centerBadges;
    layoutRequired |= scaleChanged;
    this->scale_ = ctx.scale;
    this->imageScale_ = ctx.imageScale;
    this->emoteScale_ = ctx.emoteScale;
    this->badgeScale_ = ctx.badgeScale;
    this->centerBadges_ = ctx.centerBadges;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->container_.getHeight();
    this->actuallyLayout(ctx);
    if (widthChanged || this->container_.getHeight() != oldHeight)
    {
        this->resetBuffer();
    }
    this->invalidateBuffer();

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);
    const bool hideGigantifyReward =
        this->message_->usesTwitchGigantifyPresentation();

    this->container_.beginLayout(ctx.width, this->scale_, this->imageScale_,
                                 this->emoteScale_, this->badgeScale_,
                                 this->centerBadges_, messageFlags);

    ctx.resetMessageTextCursor();
    for (const auto &element : this->message_->elements)
    {
        if (element->getFlags().has(
                MessageElementFlag::AutoModReviewExpanded) &&
            !this->flags.has(MessageLayoutFlag::AutoModReviewSelected))
        {
            continue;
        }
        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        if (hideGigantifyReward &&
            element->getFlags().has(MessageElementFlag::ChannelPointReward))
        {
            continue;
        }

        element->addToContainer(this->container_, ctx);
    }

    if (this->height_ != this->container_.getHeight())
    {
        this->resetBuffer();
    }

    this->container_.endLayout();
    this->height_ = this->container_.getHeight();

    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (this->container_.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    // The buffer is in device pixels. On a scaled screen its height is more
    // than the message takes up, and what is drawn below the message shows
    // through the next one when message backgrounds are transparent.
    const auto messageHeight = static_cast<int>(this->container_.getHeight());

    // Animated emotes aren't part of the buffer, so they get their shadow
    // here, frame by frame.
    const AnimatedMessageShadow animatedShadow{
        .color = ctx.messageShadowColor,
        .opacity = ctx.messageShadowOpacity / 100.0,
        .offset =
            QPointF(ctx.messageShadowOffset) * static_cast<qreal>(this->scale_),
        .blurRadius = std::clamp(ctx.messageShadowBlur, 0, 8) *
                      static_cast<qreal>(this->scale_),
        .emotes = ctx.messageShadowEmotes,
    };
    result.animatedRegion = this->container_.paintAnimatedElements(
        ctx.painter, ctx.y, ctx.isCollapsed,
        ctx.paintMessageShadow && ctx.messageShadowOpacity > 0 ? &animatedShadow
                                                               : nullptr);

    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                messageHeight,
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                messageHeight,
            },
            ctx.messageColors.disabled);
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        this->container_.paintSelection(ctx.painter, ctx.messageIndex,
                                        ctx.selection, ctx.y);
    }

    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                static_cast<qreal>(ctx.canvasWidth),
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->container_.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->container_.getHeight() *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull())
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QColor backgroundColor = [&] {
        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();

    // Every highlight that applies, most important first. The first one
    // colors the background, the next ones can be shown as bands.
    std::vector<QColor> highlights;
    const auto addHighlight = [&](const QColor &color) {
        if (color.isValid() &&
            std::ranges::find(highlights, color) == highlights.end())
        {
            highlights.push_back(color);
        }
    };
    const auto &prefs = ctx.preferences;
    // Goes over the background when no highlight applies.
    std::optional<QColor> replacement;

    if (this->message_->flags.has(MessageFlag::Highlighted) &&
        !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        // NOTE: As we move more things into Highlighted, and less things to use custom flags for their color,
        // they will start respecting the IgnoreHighlights flag. Keep this in mind.
        if (this->message_->highlightColor)
        {
            addHighlight(*this->message_->highlightColor);
        }
        for (const auto &color : this->message_->extraHighlightColors)
        {
            if (color)
            {
                addHighlight(*color);
            }
        }
    }
    else if (this->message_->flags.has(MessageFlag::ChatWarning))
    {
        replacement = QColor(64, 64, 64, 127);
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        replacement = QColor(0x4A273D);
    }
    else if (const auto clientColor = clientDetectionHighlightColor(
                 this->message_->clientDetection, ctx.preferences))
    {
        addHighlight(*clientColor);
    }

    if (!highlights.empty())
    {
        // Themes can make highlights stronger or fainter. Keeps a see-through
        // background see-through, so a wallpaper still shows behind it.
        backgroundColor =
            blendThemeHighlight(backgroundColor, highlights.front(),
                                ctx.highlightOpacityAdjustment);
    }
    else if (replacement)
    {
        backgroundColor = blendThemeHighlight(backgroundColor, *replacement, 0);
    }

    if (ctx.tintByPlatform)
    {
        backgroundColor = blendThemeHighlight(
            backgroundColor, platformTint(this->message_->platform), 0);
    }

    std::optional<QPixmap> clientDetectionIcon = {};

    if (getSettings()->clientDetectionIcon)
    {
        auto resources = getResources();

        switch (this->message_->clientDetection)
        {
            using enum Message::ClientDetectionStatus;
            case Webchat: {
                clientDetectionIcon = resources.chat.twitch;
                break;
            }
            case Android: {
                clientDetectionIcon = resources.chat.android;
                break;
            }
            case IOS: {
                clientDetectionIcon = resources.chat.ios;
                break;
            }
            case MobileNew: {
                clientDetectionIcon = resources.chat.mobile;
                break;
            }

            case Unknown:
            case Abnormal:
                break;
        }
    }

    painter.fillRect(buffer->rect(), backgroundColor);

    if (this->flags.has(MessageLayoutFlag::AutoModReviewSelected))
    {
        // the selected message of the AutoMod queue
        auto tint = ctx.messageColors.selection;
        tint.setAlpha(26);
        painter.fillRect(buffer->rect(), tint);

        auto accent = ctx.messageColors.linkText;
        accent.setAlpha(205);
        painter.fillRect(
            QRectF(0, 0, std::max(1.0, 2.0 * this->scale_), buffer->height()),
            accent);
    }

    if (prefs.multipleHighlightBands && highlights.size() > 1)
    {
        // The other highlights as full-color bands at the left edge.
        constexpr qreal bandWidth = 3;
        constexpr qreal bandGap = 1;
        const auto bands = std::min<size_t>(highlights.size() - 1, 2);
        for (size_t i = 0; i < bands; ++i)
        {
            auto color = highlights[i + 1];
            color.setAlpha(255);
            painter.fillRect(
                QRectF(static_cast<qreal>(i) * (bandWidth + bandGap), 0,
                       bandWidth, this->container_.getHeight()),
                color);
        }
    }

    if (getSettings()->clientDetectionIcon && clientDetectionIcon.has_value())
    {
        float size = 16 * this->scale_;
        float right =
            float(buffer->rect().right()) - (SCROLLBAR_PADDING * this->scale_);
        int left = int(right - size);
        int top = int((float(buffer->height()) - size * this->scale_) / 2);

        painter.drawPixmap(QRect(left, top, int(size), int(size)),
                           clientDetectionIcon.value());
    }

    // The content drawn on its own, when the shadow needed it anyway
    QImage contentLayer;
    if (ctx.paintMessageShadow && ctx.messageShadowOpacity > 0)
    {
        // Everything that will be drawn, in one color, blurred and moved a
        // bit: the shadow behind the text. Animated emotes are drawn on top
        // of the buffer later and get theirs there.
        // Only as wide as the content and the room the blur and the offset
        // take, not the whole width of the split: much less to blur.
        const int blurRadius = std::clamp(ctx.messageShadowBlur, 0, 8);
        const qreal reach =
            ((blurRadius * 3) + std::abs(ctx.messageShadowOffset.x()) + 4) *
            static_cast<qreal>(this->scale_);
        const auto dpr = buffer->devicePixelRatio();
        const auto shadowWidth = std::max(
            1,
            std::min(buffer->width(),
                     static_cast<int>(std::ceil(
                         (this->container_.getContentRight() + reach) * dpr))));
        const auto pixelRadius =
            qRound(blurRadius * static_cast<qreal>(this->scale_) * dpr);

        // What the shadow is made of. With emotes in the shadow that's the
        // content itself: it's drawn once, onto its own layer, and the layer
        // goes on the buffer afterwards.
        QImage source(
            QSize(ctx.messageShadowEmotes ? buffer->width() : shadowWidth,
                  buffer->height()),
            QImage::Format_ARGB32_Premultiplied);
        source.setDevicePixelRatio(dpr);
        source.fill(Qt::transparent);
        {
            QPainter sourcePainter(&source);
            sourcePainter.setRenderHint(QPainter::SmoothPixmapTransform);
            this->container_.paintElements(sourcePainter, ctx,
                                           ctx.messageShadowEmotes);
        }
        if (ctx.messageShadowEmotes)
        {
            contentLayer = source;
        }

        // The same content gives the same shadow, so it's only made again
        // when something that's drawn changed.
        size_t key = qHashBits(source.constBits(),
                               static_cast<size_t>(source.sizeInBytes()));
        for (const auto value :
             {static_cast<size_t>(ctx.messageShadowColor.rgba()),
              static_cast<size_t>(pixelRadius),
              static_cast<size_t>(shadowWidth),
              static_cast<size_t>(source.height()),
              static_cast<size_t>(dpr * 1000)})
        {
            key = qHashMulti(key, value);
        }
        if (this->shadowCache_.isNull() || this->shadowCacheKey_ != key)
        {
            QImage shadow =
                source.width() == shadowWidth
                    ? source
                    : source.copy(0, 0, shadowWidth, source.height());
            shadow.setDevicePixelRatio(dpr);
            {
                QPainter shadowPainter(&shadow);
                shadowPainter.setCompositionMode(
                    QPainter::CompositionMode_SourceIn);
                shadowPainter.fillRect(
                    QRectF(QPointF(), shadow.deviceIndependentSize()),
                    ctx.messageShadowColor);
            }
            if (pixelRadius > 0)
            {
                shadow = blurThemeShadow(std::move(shadow), pixelRadius);
            }
            this->shadowCache_ = QPixmap::fromImage(std::move(shadow));
            this->shadowCacheKey_ = key;
        }

        painter.save();
        const QPointF offset{
            ctx.messageShadowOffset.x() * static_cast<qreal>(this->scale_),
            ctx.messageShadowOffset.y() * static_cast<qreal>(this->scale_),
        };
        painter.setOpacity(ctx.messageShadowOpacity / 100.0);
        painter.drawPixmap(offset, this->shadowCache_);
        painter.restore();
    }
    else
    {
        this->shadowCache_ = {};
    }

    if (contentLayer.isNull())
    {
        this->container_.paintElements(painter, ctx);
    }
    else
    {
        painter.drawImage(QPointF(), contentLayer);
    }

#ifdef FOURTF

    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_.getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    this->shadowCache_ = {};
    this->resetBuffer();
}

void MessageLayout::resetBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();

#ifdef XD
    this->container_.clear();
#endif
}

const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    return this->container_.getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    if (hoveredElement->getWordId() != -1)
    {
        return this->container_.getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->container_.getLastCharacterIndex();
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->container_.getFirstMessageCharacterIndex();
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_.getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    this->container_.addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino
