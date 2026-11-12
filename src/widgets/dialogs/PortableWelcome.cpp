// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/PortableWelcome.hpp"

#include "Application.hpp"
#include "common/Modes.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/JilChatCard.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace chatterino {

namespace {

using namespace jilchatcard;

constexpr int CARD_WIDTH = 480;
constexpr int CARD_HEIGHT = 330;
constexpr int BUTTON_WIDTH = 200;
constexpr int BUTTON_HEIGHT = 42;

/// With the 1.0 at the start, the animations can be read from the clock
double eased(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}

}  // namespace

void PortableWelcome::showIfFirstStart()
{
    if (!getApp()->getModes().isPortable || getSettings()->portableWelcomeShown)
    {
        return;
    }
    getSettings()->portableWelcomeShown = true;
    auto *welcome = new PortableWelcome();
    welcome->show();
    welcome->raise();
}

PortableWelcome::PortableWelcome()
    : QWidget(nullptr,
              Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setAttribute(Qt::WA_QuitOnClose, false);
    this->setMouseTracking(true);
    this->setWindowTitle(QStringLiteral("Welcome to JilChat Chatterino"));
    this->setWindowIcon(QIcon(QStringLiteral(":/icon.ico")));

    this->setFixedSize(CARD_WIDTH + (SHADOW * 2), CARD_HEIGHT + (SHADOW * 2));
    this->card_ = QRectF(SHADOW, SHADOW, CARD_WIDTH, CARD_HEIGHT);
    this->controls_[GetStarted] = QRectF(
        this->card_.center().x() - (BUTTON_WIDTH / 2.0),
        this->card_.bottom() - 30 - BUTTON_HEIGHT, BUTTON_WIDTH, BUTTON_HEIGHT);
    this->controls_[Close] =
        QRectF(this->card_.right() - 38, this->card_.top() + 10, 28, 28);

    this->timer_.setTimerType(Qt::PreciseTimer);
    this->timer_.setInterval(std::chrono::milliseconds(16));
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        for (int i = 0; i < ControlCount; ++i)
        {
            const double target = this->hovered_ == i ? 1.0 : 0.0;
            this->hover_[i] += (target - this->hover_[i]) * 0.22;
        }
        this->update();
    });
    this->timer_.start();
    this->clock_.start();

    if (auto *screen = QGuiApplication::primaryScreen())
    {
        const auto area = screen->availableGeometry();
        this->move(area.center() - this->rect().center());
    }
}

void PortableWelcome::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);

    const auto &card = this->card_;
    const double now = double(this->clock_.elapsed()) / 1000.0;
    const double cx = card.center().x();
    drawCard(p, card, now, QPointF(cx, card.top() + 80), 260);

    // The logo rises in, the text and button follow
    const double rise = eased(now / 0.8);
    const double logoSide = 92;
    drawLogo(p,
             QRectF(cx - (logoSide / 2), card.top() + 28 + ((1 - rise) * 50),
                    logoSide, logoSide),
             now, rise);

    const double text = eased((now - 0.25) / 0.5);
    p.setOpacity(text);
    drawPill(p, QRectF(cx - 42, card.top() + 132, 84, 19),
             QStringLiteral("PORTABLE"), TEXT, QColor(255, 255, 255, 22),
             QColor(255, 255, 255, 90));

    p.setFont(jilchatcard::font(24, true));
    p.setPen(TEXT);
    p.drawText(QRectF(card.left(), card.top() + 158, card.width(), 34),
               Qt::AlignCenter, QStringLiteral("JilChat Chatterino"));

    p.setFont(jilchatcard::font(13, false));
    p.setPen(MUTED);
    const auto folder =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    p.drawText(
        QRectF(card.left() + 44, card.top() + 196, card.width() - 88, 40),
        Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
        QStringLiteral("Ready to go. Your settings, tabs and logs stay in this "
                       "folder, and updates install themselves."));

    p.setFont(jilchatcard::font(11, false));
    p.setPen(QColor(MUTED.red(), MUTED.green(), MUTED.blue(), 170));
    p.drawText(
        QRectF(card.left() + 30, card.top() + 238, card.width() - 60, 18),
        Qt::AlignCenter,
        QFontMetricsF(p.font()).elidedText(folder, Qt::ElideMiddle,
                                           card.width() - 60));

    const double controls = eased((now - 0.4) / 0.5);
    p.setOpacity(controls);
    drawButton(p, this->controls_[GetStarted], QStringLiteral("Get started"),
               true, this->hover_[GetStarted], false, now);
    p.setOpacity(1);
    drawClose(p, this->controls_[Close], this->hover_[Close]);
}

int PortableWelcome::controlAt(QPointF position) const
{
    for (int i = 0; i < ControlCount; ++i)
    {
        if (this->controls_[i].contains(position))
        {
            return i;
        }
    }
    return -1;
}

void PortableWelcome::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        this->move(event->globalPosition().toPoint() - this->dragOffset_);
        return;
    }
    this->hovered_ = this->controlAt(event->position());
    this->setCursor(this->hovered_ >= 0 ? Qt::PointingHandCursor
                                        : Qt::ArrowCursor);
}

void PortableWelcome::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    this->pressed_ = this->controlAt(event->position());
    if (this->pressed_ < 0 && this->card_.contains(event->position()))
    {
        this->dragging_ = true;
        this->dragOffset_ = event->globalPosition().toPoint() - this->pos();
    }
}

void PortableWelcome::mouseReleaseEvent(QMouseEvent *event)
{
    this->dragging_ = false;
    const int released = this->controlAt(event->position());
    const int pressed = this->pressed_;
    this->pressed_ = -1;
    if (released >= 0 && released == pressed)
    {
        this->close();
    }
}

void PortableWelcome::leaveEvent(QEvent * /*event*/)
{
    this->hovered_ = -1;
    this->setCursor(Qt::ArrowCursor);
}

void PortableWelcome::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter)
    {
        this->close();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace chatterino
