// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Updates.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QElapsedTimer>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <array>

namespace chatterino {

/// The update window, in the look of the setup: the new version and what it
/// changes, in sections. It follows the updater, so it shows the download and
/// what went wrong, and offers to try again. It never takes the keyboard, as
/// it can appear while someone is typing in chat.
///
/// Closing it with the X counts as "Remind me later".
class UpdatePrompt : public QWidget
{
public:
    /// Shows it, or brings the open one to the front
    static void showPrompt();
    /// Shows it once per version when an update was found, unless it was
    /// put off for this version until the next start
    static void showIfNew();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    UpdatePrompt();

    enum Control { UpdateNow, RemindLater, Close, ControlCount };

    void layOut();
    int controlAt(QPointF position) const;
    void tick();
    void statusChanged();
    /// Places the sections and changes, returning the height they need
    int layoutChanges(int width);
    void drawChanges(QPainter &painter);
    /// The changes or the message, faded at the edges while there's more to
    /// scroll
    void drawNotes(QPainter &painter);
    void drawScrollBar(QPainter &painter);
    double maxScroll() const;
    QRectF scrollTrack() const;
    QRectF scrollThumb() const;
    void scrollTo(double offset, bool instant = false);
    QString message() const;
    static bool busy();

    /// One thing to draw in the changes list, placed relative to notesArea_
    struct Piece {
        enum class Kind { Tag, Bullet, Title, Detail };
        Kind kind;
        QRectF rect;
        QString text;
        QString changeKind;
    };

    std::vector<Updates::Change> changes_;
    QVector<Piece> pieces_;
    int contentHeight_ = 0;
    bool showingMessage_ = false;

    QRectF card_;
    QRectF notesArea_;
    std::array<QRectF, ControlCount> controls_;
    std::array<double, ControlCount> hover_{};
    int hovered_ = -1;
    int pressed_ = -1;
    bool dragging_ = false;
    QPoint dragOffset_;

    double scroll_ = 0;
    double scrollTarget_ = 0;
    bool thumbHovered_ = false;
    bool draggingThumb_ = false;
    double thumbGrab_ = 0;

    QTimer timer_;
    QElapsedTimer clock_;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
