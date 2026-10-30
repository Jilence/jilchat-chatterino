// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/AvatarFrame.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Image.hpp"
#include "singletons/helper/GifTimer.hpp"

#include <QPainter>
#include <QPainterPath>

#include <chrono>

namespace chatterino {

using namespace std::chrono_literals;

bool paintAvatarWithFrame(QPainter &painter, const QRectF &rect,
                          const QPixmap &avatar, const ImagePtr &frame)
{
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QRectF avatarRect = rect;
    if (frame)
    {
        const auto side = rect.width() * AVATAR_IN_FRAME;
        avatarRect = QRectF(rect.center().x() - (side / 2),
                            rect.center().y() - (side / 2), side, side);
    }
    if (!avatar.isNull())
    {
        // Round, to sit in the frame
        painter.save();
        QPainterPath circle;
        circle.addEllipse(avatarRect);
        painter.setClipPath(circle);
        painter.drawPixmap(avatarRect, avatar, avatar.rect());
        painter.restore();
    }

    bool loaded = true;
    if (frame)
    {
        if (auto pixmap = frame->pixmapOrLoad())
        {
            painter.drawPixmap(rect, *pixmap, pixmap->rect());
        }
        else
        {
            loaded = false;
        }
    }
    painter.restore();
    return loaded;
}

AvatarFrameOverlay::AvatarFrameOverlay(QWidget *parent)
    : QWidget(parent)
{
    this->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    this->setAttribute(Qt::WA_NoSystemBackground, true);

    this->timer_.setInterval(1s);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });

    // Animated frames only move when painted again
    this->connections_.managedConnect(
        getApp()->getEmotes()->getGIFTimer()->signal, [this] {
            if (this->frame_ && this->frame_->animated() && this->isVisible())
            {
                this->update();
            }
        });
}

void AvatarFrameOverlay::setSource(std::function<ImagePtr()> source,
                                   std::function<void(bool)> onChanged)
{
    this->source_ = std::move(source);
    this->onChanged_ = std::move(onChanged);
    this->frame_ = nullptr;
    this->refresh();
    this->timer_.start();
}

void AvatarFrameOverlay::refresh()
{
    auto frame = this->source_ ? this->source_() : nullptr;
    if (frame != this->frame_)
    {
        const bool had = this->frame_ != nullptr;
        this->frame_ = std::move(frame);
        if (this->onChanged_ && had != (this->frame_ != nullptr))
        {
            this->onChanged_(this->frame_ != nullptr);
        }
        this->update();
    }
    else if (this->waiting_)
    {
        this->update();
    }
}

void AvatarFrameOverlay::paintEvent(QPaintEvent * /*event*/)
{
    if (!this->frame_)
    {
        this->waiting_ = false;
        return;
    }
    QPainter painter(this);
    this->waiting_ =
        !paintAvatarWithFrame(painter, this->rect(), {}, this->frame_);
}

}  // namespace chatterino
