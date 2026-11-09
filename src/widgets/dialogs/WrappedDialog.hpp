// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/wrapped/WrappedStats.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QPoint>
#include <QRectF>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

/// JilChat Wrapped: your month or year in chat, one slide at a time, in the
/// same style as the update card. The last slide can be saved as a picture.
class WrappedDialog : public QWidget
{
public:
    enum class Period { LastMonth, ThisMonth, ThisYear };

    static void showDialog(Period period = Period::ThisMonth);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    explicit WrappedDialog(Period period);

    enum class Slide {
        Intro,
        Nothing,
        Messages,
        Channels,
        Time,
        Emotes,
        Personality,
        People,
        Streak,
        Summary,
    };

    struct Control {
        QRectF rect;
        QString label;
        std::function<void()> action;
        bool primary = false;
        bool selected = false;
    };

    void usePeriod(Period period);
    QString periodName() const;
    /// Draws the emote's picture fitted into `box`. False when there isn't
    /// one yet, so the name can be shown instead.
    bool drawEmote(QPainter &p, const QRectF &box, const QString &name,
                   Qt::Alignment align = Qt::AlignCenter) const;
    bool showsEmotes() const;
    void goTo(int index);
    void layoutControls();
    void paintSlide(QPainter &p, const QRectF &area, Slide slide) const;
    QPixmap renderCard(double scale) const;
    void saveImage();
    void copyImage();

    wrapped::WrappedStats::Summary summary_;
    Period period_ = Period::ThisMonth;
    mutable QHash<QString, ImagePtr> emoteImages_;
    QVector<Slide> slides_;
    int index_ = 0;

    QRectF card_;
    QVector<Control> controls_;
    int hovered_ = -1;
    int pressed_ = -1;
    bool dragging_ = false;
    QPoint dragOffset_;
    QString toast_;

    /// The slide being left, fading out over the new one
    QPixmap fading_;
    QElapsedTimer fadeClock_;
    QTimer frameTimer_;
};

/// The strip across the top of the main window saying a new monthly Wrapped is ready
class WrappedBanner : public QWidget
{
public:
    /// Shows it at the top of the main window
    static void announce();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    explicit WrappedBanner(QWidget *parent);
    void layoutButtons();

    QRectF view_;
    QRectF close_;
    int hovered_ = -1;
};

}  // namespace chatterino
