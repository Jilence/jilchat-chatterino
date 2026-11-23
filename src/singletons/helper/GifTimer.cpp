// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/helper/GifTimer.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QApplication>

namespace chatterino {

void GIFTimer::updateTimer()
{
    if (!this->initialized_)
    {
        return;
    }
    const bool shouldRun = getSettings()->animateEmotes &&
                           (!getSettings()->animationsWhenFocused ||
                            this->openOverlayWindows_ != 0 ||
                            QApplication::activeWindow() != nullptr);
    if (shouldRun && !this->timer.isActive())
    {
        this->timer.start();
    }
    else if (!shouldRun)
    {
        this->timer.stop();
    }
}

void GIFTimer::initialize()
{
    this->timer.setInterval(GIF_FRAME_LENGTH);
    this->timer.setTimerType(Qt::PreciseTimer);

    this->initialized_ = true;
    getSettings()->animateEmotes.connect([this](auto, auto) {
        this->updateTimer();
    });
    getSettings()->animationsWhenFocused.connect([this](auto, auto) {
        this->updateTimer();
    });
    QObject::connect(qApp, &QApplication::focusChanged,
                     [this](QWidget *, QWidget *) {
                         this->updateTimer();
                     });
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged,
                     [this](Qt::ApplicationState) {
                         this->updateTimer();
                     });
    this->updateTimer();

    QObject::connect(&this->timer, &QTimer::timeout, [this] {
        if (getSettings()->animationsWhenFocused &&
            this->openOverlayWindows_ == 0 &&
            QApplication::activeWindow() == nullptr)
        {
            return;
        }

        this->advance();
        getApp()->getWindows()->repaintGifEmotes();
    });
}

}  // namespace chatterino
