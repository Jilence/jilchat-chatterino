// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <array>

namespace chatterino {

/// What a portable copy shows the first time it starts, in the look of the
/// setup: that it's ready, and that it keeps everything in its own folder.
class PortableWelcome : public QWidget
{
public:
    /// Shows it once per portable copy, the first time it starts
    static void showIfFirstStart();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    PortableWelcome();

    enum Control { GetStarted, Close, ControlCount };

    int controlAt(QPointF position) const;

    QRectF card_;
    std::array<QRectF, ControlCount> controls_;
    std::array<double, ControlCount> hover_{};
    int hovered_ = -1;
    int pressed_ = -1;
    bool dragging_ = false;
    QPoint dragOffset_;

    QTimer timer_;
    QElapsedTimer clock_;
};

}  // namespace chatterino
