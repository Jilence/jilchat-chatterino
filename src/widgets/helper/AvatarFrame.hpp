// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

class QPainter;

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

/// The size of a profile picture inside its frame: frames (JilTV avatar
/// decorations) are drawn at 120 % of the picture.
constexpr double AVATAR_IN_FRAME = 1 / 1.2;

/// Draws `avatar`, cut round, with `frame` around it into `rect`: the frame
/// fills it, the picture sits in the middle. Without a frame the picture fills it.
/// Returns false while the frame image is still loading.
bool paintAvatarWithFrame(QPainter &painter, const QRectF &rect,
                          const QPixmap &avatar, const ImagePtr &frame);

/// Draws a frame over its whole rect, for laying over a profile picture;
/// mouse events go through to what is below.
///
/// The frame comes from `source`, asked again every second, since who wears
/// what is looked up in the background.
class AvatarFrameOverlay : public QWidget
{
public:
    explicit AvatarFrameOverlay(QWidget *parent = nullptr);

    /// `onChanged` gets whether there is a frame now.
    void setSource(std::function<ImagePtr()> source,
                   std::function<void(bool)> onChanged = {});

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refresh();

    std::function<ImagePtr()> source_;
    std::function<void(bool)> onChanged_;
    ImagePtr frame_;
    bool waiting_ = false;
    QTimer timer_;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
