// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/Transitions.hpp"

#include "singletons/Settings.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace chatterino::transitions {

namespace {

constexpr int PAGE_MS = 230;
constexpr int CHANGE_MS = 170;
constexpr int WINDOW_IN_MS = 220;
constexpr int WINDOW_OUT_MS = 150;
constexpr int TAB_IN_MS = 200;

int runningPageTransitions = 0;

/// One frame per screen refresh: more is wasted work that the screen never
/// shows, and it slows the frames that are shown
int frameMs()
{
    double hz = 60;
    if (const auto *screen = QGuiApplication::primaryScreen())
    {
        hz = std::clamp(screen->refreshRate(), 30.0, 240.0);
    }
    return std::max(4, int(1000.0 / hz));
}

/// What `area` of the widget looks like on screen right now. The window keeps
/// the picture it last showed, so copying it is instant, while grab() has to
/// draw the whole widget again. Falls back to grab() when there's no copy.
QPixmap pictureOnScreen(QWidget *widget, QRect area = {})
{
    if (area.isNull())
    {
        area = widget->rect();
    }
    auto *window = widget->window();
    auto *store = window != nullptr ? window->backingStore() : nullptr;
    if (store != nullptr && widget->isVisible() && !widget->isMinimized())
    {
        if (const auto *image = dynamic_cast<QImage *>(store->paintDevice()))
        {
            const double dpr = image->devicePixelRatio();
            const QRect inWindow(widget->mapTo(window, area.topLeft()),
                                 area.size());
            const QRect pixels(
                QPoint(int(std::lround(inWindow.x() * dpr)),
                       int(std::lround(inWindow.y() * dpr))),
                QSize(int(std::lround(inWindow.width() * dpr)),
                      int(std::lround(inWindow.height() * dpr))));
            if (!pixels.isEmpty() && image->rect().contains(pixels))
            {
                auto picture = QPixmap::fromImage(image->copy(pixels));
                picture.setDevicePixelRatio(dpr);
                return picture;
            }
        }
    }
    return widget->grab(area);
}

/// Rounds to a whole screen pixel, so pictures are copied straight across
/// instead of being resampled, which is much slower for a whole window
QPointF onPixel(QPointF point, double dpr)
{
    return {std::round(point.x() * dpr) / dpr,
            std::round(point.y() * dpr) / dpr};
}
const auto PAGE_OVERLAY = QStringLiteral("PageTransition");
const auto CHANGE_OVERLAY = QStringLiteral("ChangeFade");

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 4.0);
}

double easeInOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Runs a function every frame for a set time. Timed by the clock, so a slow
/// frame shortens the next step instead of making the animation run long.
class Animator : public QObject
{
public:
    template <typename Step, typename Done>
    Animator(QObject *owner, int duration, Step step, Done done)
        : QObject(owner)
        , duration_(duration)
    {
        this->timer_.setTimerType(Qt::PreciseTimer);
        this->timer_.setInterval(frameMs());
        QObject::connect(
            &this->timer_, &QTimer::timeout, this,
            [this, step = std::move(step), done = std::move(done)] {
                const double t =
                    double(this->clock_.elapsed()) / this->duration_;
                step(std::min(t, 1.0));
                if (t >= 1.0)
                {
                    this->timer_.stop();
                    done();
                    this->deleteLater();
                }
            });
        this->clock_.start();
        this->timer_.start();
    }

private:
    int duration_;
    QTimer timer_;
    QElapsedTimer clock_;
};

/// A picture of a widget's old look that fades away over the new one.
class ChangeFade : public QWidget
{
public:
    ChangeFade(QWidget *host, QPixmap picture, const QRect &area)
        : QWidget(host)
        , picture_(std::move(picture))
    {
        this->setObjectName(CHANGE_OVERLAY);
        this->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->setFocusPolicy(Qt::NoFocus);
        this->setGeometry(area);
        this->show();
        this->raise();

        // Starts after the change, which may add widgets above this one
        QTimer::singleShot(0, this, [this] {
            this->raise();
            new Animator(
                this, CHANGE_MS,
                [this](double t) {
                    this->opacity_ = 1.0 - easeInOut(t);
                    this->update();
                },
                [this] {
                    this->hide();
                    this->deleteLater();
                });
        });
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setOpacity(this->opacity_);
        painter.drawPixmap(this->rect(), this->picture_);
    }

private:
    QPixmap picture_;
    double opacity_ = 1.0;
};

/// Covers the pages while they glide across. Only pictures get redrawn, and the
/// real pages underneath are left alone until it's over.
class PageTransition : public QWidget
{
public:
    PageTransition(QWidget *host, QPixmap oldPicture, QWidget *newPage,
                   const QRect &area, PageDirection direction, bool vertical)
        : QWidget(host)
        , oldPicture_(std::move(oldPicture))
        , newPage_(newPage)
    {
        this->setObjectName(PAGE_OVERLAY);
        this->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->setAttribute(Qt::WA_OpaquePaintEvent);
        this->setFocusPolicy(Qt::NoFocus);
        this->setGeometry(area);

        const int sign = direction == PageDirection::Forward ? 1 : -1;
        const auto distance = vertical ? std::min(area.height() / 10, 60)
                                       : std::min(area.width() / 8, 96);
        this->travel_ = vertical ? QPointF(0, sign * distance)
                                 : QPointF(sign * distance, 0);
        this->show();
        this->raise();

        // The switch finishes first, so the new page is laid out when pictured
        QTimer::singleShot(0, this, [this] {
            this->begin();
        });
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        if (this->newPicture_.isNull())
        {
            painter.drawPixmap(this->rect(), this->oldPicture_);
            return;
        }

        const double moved = easeOut(this->progress_);
        const double blend = easeInOut(this->progress_ * 1.15);
        painter.fillRect(this->rect(), this->background_);

        // Whole screen pixels: a straight copy, which keeps up at any frame
        // rate, where in-between positions would need slow resampling
        const double dpr = this->devicePixelRatioF();
        painter.setOpacity(blend);
        painter.drawPixmap(onPixel(this->travel_ * (1.0 - moved), dpr),
                           this->newPicture_);
        painter.setOpacity(1.0 - blend);
        painter.drawPixmap(onPixel(this->travel_ * -moved, dpr),
                           this->oldPicture_);
    }

private:
    void begin()
    {
        if (this->newPage_ != nullptr)
        {
            if (this->newPage_->geometry() != this->geometry())
            {
                this->newPage_->setGeometry(this->geometry());
            }
            this->newPicture_ = this->newPage_->grab();
        }
        if (this->newPicture_.isNull())
        {
            this->hide();
            this->deleteLater();
            return;
        }

        // The gap the pages leave at the edge matches the new page
        this->background_ =
            this->newPicture_.copy(0, 0, 1, 1).toImage().pixelColor(0, 0);

        // Anything the switch added above this goes back underneath
        this->raise();
        this->counted_ = true;
        ++runningPageTransitions;
        new Animator(
            this, PAGE_MS,
            [this](double t) {
                this->progress_ = t;
                this->update();
            },
            [this] {
                this->hide();
                this->deleteLater();
            });
    }

public:
    ~PageTransition() override
    {
        if (this->counted_)
        {
            --runningPageTransitions;
        }
    }

private:
    QPixmap oldPicture_;
    QPixmap newPicture_;
    QPointer<QWidget> newPage_;
    QPointF travel_;
    QColor background_;
    double progress_ = 0;
    bool counted_ = false;
};

/// A window's last look, left on screen for a moment after it has closed.
class ClosingPicture : public QWidget
{
public:
    ClosingPicture(QPixmap picture, const QRect &area)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::Tool | Qt::WindowTransparentForInput |
                               Qt::WindowDoesNotAcceptFocus |
                               Qt::NoDropShadowWindowHint)
        , picture_(std::move(picture))
    {
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->setAttribute(Qt::WA_ShowWithoutActivating);
        this->setAttribute(Qt::WA_QuitOnClose, false);
        this->setGeometry(area);
        this->show();

        const auto start = this->pos();
        new Animator(
            this, WINDOW_OUT_MS,
            [this, start](double t) {
                this->setWindowOpacity(1.0 - easeInOut(t));
                this->move(start +
                           QPoint(0, int(std::lround(easeInOut(t) * 8))));
            },
            [this] {
                this->hide();
                this->deleteLater();
            });
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawPixmap(this->rect(), this->picture_);
    }

private:
    QPixmap picture_;
};

void applyEffects(bool enabled)
{
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, enabled);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, enabled);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, enabled);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, enabled);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, enabled);
}

bool skipsWindowAnimation(QWidget *window)
{
    return window == nullptr || !window->isWindow() ||
           !smoothAnimationsEnabled() ||
           window->property("noAnimation").toBool();
}

}  // namespace

bool smoothAnimationsEnabled()
{
    return getSettings()->smoothAnimations;
}

bool pageTransitionRunning()
{
    return runningPageTransitions > 0;
}

void slidePages(QWidget *oldPage, QWidget *newPage, PageDirection direction,
                bool vertical)
{
    if (oldPage == nullptr || newPage == nullptr || oldPage == newPage ||
        !smoothAnimationsEnabled() || !oldPage->isVisible() ||
        oldPage->size().isEmpty() || oldPage->parentWidget() == nullptr)
    {
        return;
    }
    auto *host = oldPage->parentWidget();

    // Switching again mid-way carries on from what's on screen right now
    QPixmap picture;
    for (auto *running : host->findChildren<QWidget *>(
             PAGE_OVERLAY, Qt::FindDirectChildrenOnly))
    {
        if (picture.isNull() && running->isVisible())
        {
            picture = pictureOnScreen(running);
        }
        running->hide();
        running->deleteLater();
    }
    if (picture.isNull())
    {
        picture = pictureOnScreen(oldPage);
    }

    new PageTransition(host, std::move(picture), newPage, oldPage->geometry(),
                       direction, vertical);
}

void fadeChange(QWidget *widget, bool includeBehind)
{
    if (widget == nullptr || !smoothAnimationsEnabled() ||
        !widget->isVisible() || widget->size().isEmpty())
    {
        return;
    }

    // What's on screen already has whatever is behind the widget in it
    QPixmap picture = includeBehind ? pictureOnScreen(widget) : widget->grab();

    for (auto *running : widget->findChildren<QWidget *>(
             CHANGE_OVERLAY, Qt::FindDirectChildrenOnly))
    {
        running->hide();
        running->deleteLater();
    }
    new ChangeFade(widget, std::move(picture), widget->rect());
}

void fadeInWidget(QWidget *widget)
{
    if (widget == nullptr || !smoothAnimationsEnabled())
    {
        return;
    }

    auto *effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0.0);
    widget->setGraphicsEffect(effect);
    QPointer<QWidget> target(widget);
    new Animator(
        effect, TAB_IN_MS,
        [effect](double t) {
            effect->setOpacity(easeInOut(t));
        },
        [target] {
            if (target)
            {
                // Also deletes the effect, and this along with it
                QTimer::singleShot(0, target, [target] {
                    if (target)
                    {
                        target->setGraphicsEffect(nullptr);
                    }
                });
            }
        });
}

void fadeInWindow(QWidget *window, int riseBy)
{
    if (skipsWindowAnimation(window))
    {
        return;
    }
    if (window->isMaximized() || window->isFullScreen())
    {
        riseBy = 0;
    }

    window->setWindowOpacity(0.0);

    // Waits for the first paint: a window that's slow to open would otherwise
    // use up its fade before anything can be seen.
    QPointer<QWidget> target(window);
    QTimer::singleShot(20, window, [target, riseBy] {
        if (!target)
        {
            return;
        }
        // Moves by the difference, so it can't undo a move made meanwhile
        auto offset = std::make_shared<int>(riseBy);
        if (riseBy != 0)
        {
            target->move(target->pos() + QPoint(0, riseBy));
        }
        const auto apply = [target, offset](int next) {
            if (target && next != *offset)
            {
                target->move(target->pos() + QPoint(0, next - *offset));
                *offset = next;
            }
        };
        new Animator(
            target, WINDOW_IN_MS,
            [target, riseBy, apply](double t) {
                if (target)
                {
                    target->setWindowOpacity(easeInOut(t * 1.4));
                    apply(int(std::lround((1.0 - easeOut(t)) * riseBy)));
                }
            },
            [target, apply] {
                if (target)
                {
                    apply(0);
                    target->setWindowOpacity(1.0);
                }
            });
    });
}

void fadeOutWindow(QWidget *window)
{
    if (skipsWindowAnimation(window) || !window->isVisible() ||
        window->isMinimized() || window->size().isEmpty() ||
        QCoreApplication::closingDown())
    {
        return;
    }
    new ClosingPicture(pictureOnScreen(window), window->geometry());
}

void initializeAnimationEffects()
{
    // Lives as long as the app does
    static auto *connections = new pajlada::Signals::SignalHolder();
    getSettings()->smoothAnimations.connect(
        [](const bool &enabled, auto) {
            applyEffects(enabled);
        },
        *connections);
}

}  // namespace chatterino::transitions
