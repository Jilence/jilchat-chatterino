// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/WrappedDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/Transitions.hpp"
#include "widgets/Window.hpp"

#include <QBoxLayout>
#include <QClipboard>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace chatterino {

namespace {

constexpr int CARD_WIDTH = 440;
constexpr int CARD_HEIGHT = 600;
constexpr int SHADOW = 24;
constexpr double RADIUS = 18;
constexpr int PAD = 30;
constexpr int FADE_MS = 240;

const QColor WHITE(248, 241, 239);
const QColor MUTED(186, 164, 162);

QPointer<WrappedDialog> openDialog;

QFont uiFont(double pixels, bool strong)
{
    QFont font(strong ? QStringLiteral("Segoe UI Semibold")
                      : QStringLiteral("Segoe UI"));
    font.setPixelSize(int(std::lround(pixels)));
    return font;
}

QString number(int value)
{
    return QLocale().toString(value);
}

/// Each slide has its own glow color, so they feel different as you go
QColor accentFor(int slide)
{
    static const QColor accents[] = {
        QColor(227, 30, 44),  QColor(227, 30, 44),   QColor(236, 72, 153),
        QColor(59, 130, 246), QColor(20, 184, 166),  QColor(234, 179, 8),
        QColor(168, 85, 247), QColor(244, 114, 182), QColor(249, 115, 22),
        QColor(227, 30, 44),
    };
    return accents[std::clamp(slide, 0, 9)];
}

QString hourName(int hour)
{
    return QTime(hour, 0).toString(QStringLiteral("h AP"));
}

struct Personality {
    QString title;
    QString line;
};

Personality personalityFor(const std::array<int, 24> &hours)
{
    const auto peak =
        int(std::max_element(hours.begin(), hours.end()) - hours.begin());
    const auto at = hourName(peak);
    if (peak < 5)
    {
        return {QStringLiteral("Night Owl"),
                QStringLiteral("You chat most around %1. The best chats happen "
                               "after midnight.")
                    .arg(at)};
    }
    if (peak < 10)
    {
        return {QStringLiteral("Early Bird"),
                QStringLiteral("You're in chat by %1, before most people are "
                               "even awake.")
                    .arg(at)};
    }
    if (peak < 14)
    {
        return {QStringLiteral("Lunch Break Chatter"),
                QStringLiteral("Your chat time peaks around %1. Snacks and "
                               "streams.")
                    .arg(at)};
    }
    if (peak < 18)
    {
        return {QStringLiteral("Afternoon Regular"),
                QStringLiteral("You show up around %1, right as streams get "
                               "going.")
                    .arg(at)};
    }
    if (peak < 22)
    {
        return {QStringLiteral("Prime Timer"),
                QStringLiteral("You chat most around %1, when streams are at "
                               "their biggest.")
                    .arg(at)};
    }
    return {
        QStringLiteral("Late Night Lurker"),
        QStringLiteral("Around %1 is when you come alive in chat.").arg(at)};
}

QString hoursText(int minutes)
{
    if (minutes < 60)
    {
        return QStringLiteral("%1 min").arg(minutes);
    }
    const int hours = minutes / 60;
    const int rest = minutes % 60;
    return rest == 0
               ? QStringLiteral("%1 h").arg(number(hours))
               : QStringLiteral("%1 h %2 min").arg(number(hours)).arg(rest);
}

QString accountName()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    return account == nullptr || account->isAnon() ? QString()
                                                   : account->getUserName();
}

void drawCentered(QPainter &p, const QRectF &rect, const QString &text,
                  const QFont &font, const QColor &color,
                  int flags = Qt::AlignCenter | Qt::TextWordWrap)
{
    p.setFont(font);
    p.setPen(color);
    p.drawText(rect, flags, text);
}

/// A big number that fades from the slide's color into white
void drawBigNumber(QPainter &p, const QRectF &rect, const QString &text,
                   const QColor &accent, double size = 72)
{
    auto font = uiFont(size, true);
    QFontMetricsF metrics(font);
    while (metrics.horizontalAdvance(text) > rect.width() && size > 24)
    {
        size -= 4;
        font = uiFont(size, true);
        metrics = QFontMetricsF(font);
    }
    QLinearGradient fill(rect.topLeft(), rect.bottomLeft());
    fill.setColorAt(0, QColor(255, 255, 255));
    fill.setColorAt(1, accent.lighter(135));
    QPen pen;
    pen.setBrush(fill);
    p.setPen(pen);
    p.setFont(font);
    p.drawText(rect, Qt::AlignCenter, text);
}

using DrawIcon =
    std::function<bool(QPainter &, const QRectF &, const QString &)>;

void drawRanking(QPainter &p, QRectF area,
                 const wrapped::WrappedStats::Ranking &ranking,
                 const QColor &accent, const QString &prefix,
                 const QString &unit, int firstRank = 1,
                 const DrawIcon &drawIcon = {})
{
    if (ranking.empty())
    {
        return;
    }
    const double rowHeight =
        std::min(52.0, area.height() / double(ranking.size()));
    const int best = std::max(1, ranking.front().second);
    for (size_t i = 0; i < ranking.size(); ++i)
    {
        const QRectF row(area.left(), area.top() + (i * rowHeight),
                         area.width(), rowHeight - 8);
        QPainterPath back;
        back.addRoundedRect(row, 10, 10);
        p.fillPath(back, QColor(255, 255, 255, i == 0 ? 22 : 12));

        // How it compares with the top one
        QRectF bar = row;
        bar.setWidth(std::max(12.0, row.width() * ranking[i].second / best));
        QPainterPath barPath;
        barPath.addRoundedRect(bar, 10, 10);
        auto barColor = accent;
        barColor.setAlpha(i == 0 ? 90 : 45);
        p.fillPath(barPath, barColor);

        p.setFont(uiFont(15, true));
        p.setPen(i == 0 ? WHITE : MUTED);
        p.drawText(row.adjusted(12, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::number(int(i) + firstRank));
        const auto name = prefix + ranking[i].first;
        const QRectF iconBox(row.left() + 38, row.top() + 5, row.width() - 128,
                             row.height() - 10);
        if (!drawIcon || !drawIcon(p, iconBox, ranking[i].first))
        {
            p.setFont(uiFont(i == 0 ? 17 : 15, true));
            p.setPen(WHITE);
            p.drawText(row.adjusted(38, 0, -90, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetricsF(p.font()).elidedText(name, Qt::ElideRight,
                                                          row.width() - 128));
        }
        p.setFont(uiFont(13, false));
        p.setPen(MUTED);
        p.drawText(row.adjusted(0, 0, -12, 0),
                   Qt::AlignRight | Qt::AlignVCenter,
                   number(ranking[i].second) + unit);
    }
}

}  // namespace

void WrappedDialog::showDialog(Period period)
{
    if (openDialog)
    {
        openDialog->usePeriod(period);
        openDialog->raise();
        openDialog->activateWindow();
        return;
    }
    openDialog = new WrappedDialog(period);
    openDialog->show();
    openDialog->activateWindow();
    openDialog->setFocus();
}

WrappedDialog::WrappedDialog(Period period)
    : QWidget(nullptr,
              Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setMouseTracking(true);
    this->setFocusPolicy(Qt::StrongFocus);
    this->setWindowTitle(QStringLiteral("JilChat Wrapped"));
    this->setFixedSize(CARD_WIDTH + SHADOW * 2, CARD_HEIGHT + SHADOW * 2);
    this->card_ = QRectF(SHADOW, SHADOW, CARD_WIDTH, CARD_HEIGHT);

    this->frameTimer_.setTimerType(Qt::PreciseTimer);
    this->frameTimer_.setInterval(16);
    QObject::connect(&this->frameTimer_, &QTimer::timeout, this, [this] {
        if (!this->fading_.isNull() && this->fadeClock_.elapsed() >= FADE_MS)
        {
            this->fading_ = {};
        }
        // Animated emotes keep moving; otherwise there's nothing to redraw
        if (this->fading_.isNull())
        {
            if (this->showsEmotes())
            {
                this->frameTimer_.setInterval(40);
            }
            else
            {
                this->frameTimer_.stop();
            }
        }
        this->update();
    });

    // Anything counted in the last few minutes is included
    wrapped::WrappedStats::instance().save();
    this->usePeriod(period);

    if (auto *screen = QGuiApplication::primaryScreen())
    {
        this->move(screen->availableGeometry().center() -
                   this->rect().center());
    }
    transitions::fadeInWindow(this, 18);
}

void WrappedDialog::usePeriod(Period period)
{
    this->period_ = period;
    const auto today = QDate::currentDate();
    const QDate thisMonth(today.year(), today.month(), 1);
    QDate from;
    QDate to;
    switch (period)
    {
        case Period::LastMonth:
            from = thisMonth.addMonths(-1);
            to = thisMonth.addDays(-1);
            break;
        case Period::ThisMonth:
            from = thisMonth;
            to = thisMonth.addMonths(1).addDays(-1);
            break;
        case Period::ThisYear:
            from = QDate(today.year(), 1, 1);
            to = QDate(today.year(), 12, 31);
            break;
    }
    this->summary_ = wrapped::WrappedStats::instance().summarize(from, to);

    this->slides_ = {Slide::Intro};
    const auto &s = this->summary_;
    if (s.empty())
    {
        this->slides_.push_back(Slide::Nothing);
    }
    else
    {
        if (s.messages > 0)
        {
            this->slides_.push_back(Slide::Messages);
            this->slides_.push_back(Slide::Channels);
        }
        if (s.minutes > 0)
        {
            this->slides_.push_back(Slide::Time);
        }
        if (!s.emotes.empty())
        {
            this->slides_.push_back(Slide::Emotes);
        }
        if (s.messages > 0)
        {
            this->slides_.push_back(Slide::Personality);
        }
        if (!s.fans.empty() || !s.talkedTo.empty())
        {
            this->slides_.push_back(Slide::People);
        }
        if (s.activeDays > 0)
        {
            this->slides_.push_back(Slide::Streak);
        }
        this->slides_.push_back(Slide::Summary);
    }
    this->index_ = 0;
    this->layoutControls();
    this->update();
}

QString WrappedDialog::periodName() const
{
    const auto &from = this->summary_.from;
    if (this->period_ == Period::ThisYear)
    {
        return QString::number(from.year());
    }
    const auto month = QLocale().standaloneMonthName(from.month());
    return from.year() == QDate::currentDate().year()
               ? month
               : QStringLiteral("%1 %2").arg(month).arg(from.year());
}

bool WrappedDialog::showsEmotes() const
{
    const auto slide = this->slides_.value(this->index_);
    return slide == Slide::Emotes || slide == Slide::Summary;
}

bool WrappedDialog::drawEmote(QPainter &p, const QRectF &box,
                              const QString &name, Qt::Alignment align) const
{
    auto image = this->emoteImages_.value(name);
    if (image == nullptr)
    {
        const auto url = wrapped::WrappedStats::instance().emoteUrl(name);
        if (url.isEmpty())
        {
            return false;
        }
        image = Image::fromUrl(Url{url});
        this->emoteImages_.insert(name, image);
    }
    const auto pixmap = image->pixmapOrLoad();
    if (!pixmap || pixmap->isNull())
    {
        return false;
    }

    // As big as fits, without stretching it
    QSizeF size = pixmap->deviceIndependentSize();
    size.scale(box.size(), Qt::KeepAspectRatio);
    QPointF at(box.left(), box.center().y() - (size.height() / 2));
    if (align & Qt::AlignHCenter)
    {
        at.setX(box.center().x() - (size.width() / 2));
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(QRectF(at, size), *pixmap, QRectF(pixmap->rect()));
    p.restore();
    return true;
}

void WrappedDialog::goTo(int index)
{
    index = std::clamp(index, 0, int(this->slides_.size()) - 1);
    if (index == this->index_)
    {
        return;
    }
    if (transitions::smoothAnimationsEnabled())
    {
        this->fading_ = this->renderCard(this->devicePixelRatioF());
        this->fadeClock_.start();
    }
    this->index_ = index;
    this->toast_.clear();
    this->layoutControls();
    if (!this->fading_.isNull() || this->showsEmotes())
    {
        this->frameTimer_.setInterval(this->fading_.isNull() ? 40 : 16);
        this->frameTimer_.start();
    }
    this->update();
}

void WrappedDialog::layoutControls()
{
    this->controls_.clear();
    this->hovered_ = -1;
    const auto &card = this->card_;
    const double rowTop = card.bottom() - PAD - 38;
    const double right = card.right() - PAD;
    const auto slide = this->slides_.value(this->index_);
    const bool last = this->index_ == int(this->slides_.size()) - 1;

    this->controls_.push_back(
        {QRectF(card.right() - 42, card.top() + 12, 30, 30),
         QStringLiteral("×"), [this] {
             transitions::fadeOutWindow(this);
             this->close();
         }});

    if (slide == Slide::Intro)
    {
        const double width = 112;
        const double gap = 6;
        const double top = card.top() + 360;
        double left = card.center().x() - (width * 1.5) - gap;
        const std::pair<QString, Period> choices[] = {
            {QStringLiteral("Last month"), Period::LastMonth},
            {QStringLiteral("This month"), Period::ThisMonth},
            {QStringLiteral("This year"), Period::ThisYear},
        };
        for (const auto &[label, period] : choices)
        {
            this->controls_.push_back({QRectF(left, top, width, 34), label,
                                       [this, period] {
                                           this->usePeriod(period);
                                       },
                                       false, this->period_ == period});
            left += width + gap;
        }
    }

    if (this->index_ > 0)
    {
        this->controls_.push_back({QRectF(card.left() + PAD, rowTop, 96, 38),
                                   QStringLiteral("Back"), [this] {
                                       this->goTo(this->index_ - 1);
                                   }});
    }
    if (slide == Slide::Summary)
    {
        this->controls_.push_back(
            {QRectF(right - 130 - 8 - 100, rowTop, 100, 38),
             QStringLiteral("Copy"), [this] {
                 this->copyImage();
             }});
        this->controls_.push_back({QRectF(right - 130, rowTop, 130, 38),
                                   QStringLiteral("Save image"),
                                   [this] {
                                       this->saveImage();
                                   },
                                   true});
    }
    else if (!last)
    {
        this->controls_.push_back({QRectF(right - 130, rowTop, 130, 38),
                                   this->index_ == 0
                                       ? QStringLiteral("Let's go")
                                       : QStringLiteral("Next"),
                                   [this] {
                                       this->goTo(this->index_ + 1);
                                   },
                                   true});
    }
    else
    {
        this->controls_.push_back({QRectF(right - 130, rowTop, 130, 38),
                                   QStringLiteral("Done"),
                                   [this] {
                                       transitions::fadeOutWindow(this);
                                       this->close();
                                   },
                                   true});
    }
}

void WrappedDialog::paintSlide(QPainter &p, const QRectF &card,
                               Slide slide) const
{
    const auto &s = this->summary_;
    const auto accent = accentFor(int(slide));
    const QRectF body = card.adjusted(PAD, PAD + 26, -PAD, -PAD - 70);
    const QString period = this->periodName();

    // Small heading over every slide but the first
    const auto heading = [&](const QString &text) {
        drawCentered(p, QRectF(body.left(), body.top(), body.width(), 22),
                     text.toUpper(), uiFont(12, true), accent.lighter(150));
    };

    switch (slide)
    {
        case Slide::Intro: {
            const QRectF pill(card.center().x() - 75, card.top() + 96, 150, 24);
            QPainterPath pillPath;
            pillPath.addRoundedRect(pill, 12, 12);
            p.fillPath(pillPath, QColor(150, 12, 22, 130));
            p.setPen(QPen(QColor(255, 80, 84, 120), 1));
            p.drawPath(pillPath);
            drawCentered(p, pill, QStringLiteral("JILCHAT WRAPPED"),
                         uiFont(11, true), QColor(255, 196, 194));

            drawCentered(
                p, QRectF(body.left(), card.top() + 150, body.width(), 40),
                QStringLiteral("Your"), uiFont(24, false), MUTED);
            drawBigNumber(
                p, QRectF(body.left(), card.top() + 185, body.width(), 90),
                period, accent, 64);
            drawCentered(
                p, QRectF(body.left(), card.top() + 275, body.width(), 40),
                QStringLiteral("in chat"), uiFont(24, false), MUTED);

            if (s.countingSince.isValid())
            {
                drawCentered(
                    p, QRectF(body.left(), card.top() + 420, body.width(), 40),
                    QStringLiteral("JilChat Chatterino started counting on %1.")
                        .arg(QLocale().toString(s.countingSince,
                                                QLocale::LongFormat)),
                    uiFont(12, false),
                    QColor(MUTED.red(), MUTED.green(), MUTED.blue(), 170));
            }
            break;
        }

        case Slide::Nothing:
            heading(QStringLiteral("Not much yet"));
            drawCentered(
                p, body.adjusted(10, 80, -10, -80),
                QStringLiteral(
                    "JilChat Chatterino only just started counting.\n\n"
                    "Chat for a while and come back: your "
                    "messages, channels, emotes and more "
                    "will show up here."),
                uiFont(17, false), WHITE);
            break;

        case Slide::Messages:
            heading(QStringLiteral("Messages"));
            drawCentered(p,
                         QRectF(body.left(), body.top() + 70, body.width(), 30),
                         QStringLiteral("You sent"), uiFont(20, false), MUTED);
            drawBigNumber(
                p, QRectF(body.left(), body.top() + 105, body.width(), 100),
                number(s.messages), accent, 84);
            drawCentered(
                p, QRectF(body.left(), body.top() + 205, body.width(), 30),
                s.messages == 1 ? QStringLiteral("message")
                                : QStringLiteral("messages"),
                uiFont(20, false), MUTED);
            drawCentered(
                p, QRectF(body.left(), body.top() + 260, body.width(), 30),
                QStringLiteral("in %1 %2")
                    .arg(s.channels.size())
                    .arg(s.channels.size() == 1 ? QStringLiteral("channel")
                                                : QStringLiteral("channels")),
                uiFont(16, true), WHITE);
            if (s.busiestDay.isValid())
            {
                drawCentered(
                    p, QRectF(body.left(), body.top() + 330, body.width(), 60),
                    QStringLiteral(
                        "Your busiest day was %1,\nwith %2 messages.")
                        .arg(QLocale().toString(s.busiestDay,
                                                QStringLiteral("dddd, MMMM d")))
                        .arg(number(s.busiestDayMessages)),
                    uiFont(15, false), MUTED);
            }
            break;

        case Slide::Channels:
            heading(QStringLiteral("Where you chatted"));
            drawCentered(
                p, QRectF(body.left(), body.top() + 40, body.width(), 60),
                QStringLiteral("Your top channels"), uiFont(26, true), WHITE);
            drawRanking(p, body.adjusted(0, 120, 0, 0), s.channels, accent,
                        QStringLiteral("#"), QStringLiteral(" msgs"));
            break;

        case Slide::Time: {
            heading(QStringLiteral("Time in chat"));
            drawCentered(p,
                         QRectF(body.left(), body.top() + 60, body.width(), 30),
                         QStringLiteral("You had chat open for"),
                         uiFont(20, false), MUTED);
            drawBigNumber(
                p, QRectF(body.left(), body.top() + 95, body.width(), 100),
                hoursText(s.minutes), accent, 64);
            if (!s.watched.empty())
            {
                drawCentered(
                    p, QRectF(body.left(), body.top() + 230, body.width(), 30),
                    QStringLiteral("Most of it in"), uiFont(16, false), MUTED);
                drawCentered(
                    p, QRectF(body.left(), body.top() + 258, body.width(), 40),
                    QStringLiteral("#") + s.watched.front().first,
                    uiFont(28, true), WHITE);
                drawCentered(
                    p, QRectF(body.left(), body.top() + 300, body.width(), 30),
                    hoursText(s.watched.front().second), uiFont(15, false),
                    MUTED);
            }
            break;
        }

        case Slide::Emotes:
            heading(QStringLiteral("Emotes"));
            drawCentered(
                p, QRectF(body.left(), body.top() + 40, body.width(), 30),
                QStringLiteral("Your go-to emote"), uiFont(18, false), MUTED);
            {
                const QRectF hero(body.left(), body.top() + 70, body.width(),
                                  70);
                if (!this->drawEmote(p, hero.adjusted(0, 2, 0, -2),
                                     s.emotes.front().first))
                {
                    drawBigNumber(p, hero, s.emotes.front().first, accent, 48);
                }
            }
            drawCentered(
                p, QRectF(body.left(), body.top() + 140, body.width(), 26),
                QStringLiteral("used in %1 messages")
                    .arg(number(s.emotes.front().second)),
                uiFont(14, false), MUTED);
            if (s.emotes.size() > 1)
            {
                const wrapped::WrappedStats::Ranking rest(s.emotes.begin() + 1,
                                                          s.emotes.end());
                drawRanking(p, body.adjusted(0, 190, 0, 0), rest, accent, {},
                            QStringLiteral("×"), 2,
                            [this](QPainter &painter, const QRectF &box,
                                   const QString &name) {
                                return this->drawEmote(painter, box, name,
                                                       Qt::AlignLeft);
                            });
            }
            break;

        case Slide::Personality: {
            heading(QStringLiteral("Your chat personality"));
            const auto who = personalityFor(s.hours);
            drawBigNumber(
                p, QRectF(body.left(), body.top() + 50, body.width(), 80),
                who.title, accent, 44);
            drawCentered(p,
                         QRectF(body.left() + 10, body.top() + 130,
                                body.width() - 20, 60),
                         who.line, uiFont(15, false), MUTED);

            // When you chat, hour by hour
            const QRectF chart(body.left(), body.top() + 230, body.width(),
                               120);
            const int peak =
                std::max(1, *std::max_element(s.hours.begin(), s.hours.end()));
            const double slot = chart.width() / 24;
            for (int hour = 0; hour < 24; ++hour)
            {
                const double height = std::max(
                    3.0, chart.height() * s.hours[size_t(hour)] / peak);
                const QRectF bar(chart.left() + (hour * slot) + 1.5,
                                 chart.bottom() - height, slot - 3, height);
                QPainterPath path;
                path.addRoundedRect(bar, 2.5, 2.5);
                auto color = accent;
                color.setAlpha(s.hours[size_t(hour)] == peak ? 230 : 90);
                p.fillPath(path, color);
            }
            p.setFont(uiFont(11, false));
            p.setPen(MUTED);
            for (int hour : {0, 6, 12, 18})
            {
                p.drawText(QRectF(chart.left() + (hour * slot) - 10,
                                  chart.bottom() + 6, 50, 16),
                           Qt::AlignLeft, hourName(hour));
            }
            break;
        }

        case Slide::People: {
            heading(QStringLiteral("Your people"));
            double y = body.top() + 50;
            if (!s.fans.empty())
            {
                drawCentered(p, QRectF(body.left(), y, body.width(), 26),
                             QStringLiteral("Your biggest fan"),
                             uiFont(17, false), MUTED);
                drawBigNumber(p, QRectF(body.left(), y + 26, body.width(), 60),
                              s.fans.front().first, accent, 40);
                drawCentered(p, QRectF(body.left(), y + 88, body.width(), 24),
                             QStringLiteral("mentioned you %1 times")
                                 .arg(number(s.fans.front().second)),
                             uiFont(14, false), MUTED);
                y += 150;
            }
            if (!s.talkedTo.empty())
            {
                drawCentered(p, QRectF(body.left(), y, body.width(), 26),
                             QStringLiteral("You talked to most"),
                             uiFont(17, false), MUTED);
                drawBigNumber(p, QRectF(body.left(), y + 26, body.width(), 60),
                              s.talkedTo.front().first, accent, 40);
                drawCentered(p, QRectF(body.left(), y + 88, body.width(), 24),
                             QStringLiteral("%1 messages to them")
                                 .arg(number(s.talkedTo.front().second)),
                             uiFont(14, false), MUTED);
            }
            if (s.mentions > 0)
            {
                drawCentered(
                    p,
                    QRectF(body.left(), body.bottom() - 40, body.width(), 30),
                    QStringLiteral("People mentioned you %1 times in all.")
                        .arg(number(s.mentions)),
                    uiFont(14, false), MUTED);
            }
            break;
        }

        case Slide::Streak:
            heading(QStringLiteral("Showing up"));
            drawCentered(p,
                         QRectF(body.left(), body.top() + 60, body.width(), 30),
                         QStringLiteral("Your longest chat streak"),
                         uiFont(20, false), MUTED);
            drawBigNumber(
                p, QRectF(body.left(), body.top() + 95, body.width(), 100),
                number(s.longestStreak), accent, 88);
            drawCentered(
                p, QRectF(body.left(), body.top() + 195, body.width(), 30),
                s.longestStreak == 1 ? QStringLiteral("day in a row")
                                     : QStringLiteral("days in a row"),
                uiFont(20, false), MUTED);
            drawCentered(
                p, QRectF(body.left(), body.top() + 270, body.width(), 60),
                QStringLiteral("You chatted on %1 of the %2 days\nthat were "
                               "counted.")
                    .arg(s.activeDays)
                    .arg(std::max(s.activeDays, s.daysInRange)),
                uiFont(15, false), WHITE);
            break;

        case Slide::Summary: {
            const auto name = accountName();
            drawCentered(
                p, QRectF(body.left(), body.top() - 6, body.width(), 30),
                name.isEmpty()
                    ? QStringLiteral("My %1 on JilChat").arg(period)
                    : QStringLiteral("%1's %2 on JilChat").arg(name, period),
                uiFont(20, true), WHITE);

            struct Tile {
                QString label;
                QString value;
            };
            QVector<Tile> tiles{
                {QStringLiteral("Messages"), number(s.messages)},
                {QStringLiteral("Chat open"), hoursText(s.minutes)},
                {QStringLiteral("Top channel"),
                 s.channels.empty()
                     ? QStringLiteral("-")
                     : QStringLiteral("#") + s.channels.front().first},
                {QStringLiteral("Top emote"), s.emotes.empty()
                                                  ? QStringLiteral("-")
                                                  : s.emotes.front().first},
                {QStringLiteral("Longest streak"),
                 QStringLiteral("%1 days").arg(s.longestStreak)},
                {QStringLiteral("Personality"),
                 s.messages > 0 ? personalityFor(s.hours).title
                                : QStringLiteral("-")},
            };
            const double gap = 10;
            const double tileWidth = (body.width() - gap) / 2;
            const double tileHeight = 92;
            for (int i = 0; i < tiles.size(); ++i)
            {
                const QRectF tile(
                    body.left() + ((i % 2) * (tileWidth + gap)),
                    body.top() + 46 + ((i / 2) * (tileHeight + gap)), tileWidth,
                    tileHeight);
                QPainterPath path;
                path.addRoundedRect(tile, 14, 14);
                QLinearGradient fill(tile.topLeft(), tile.bottomRight());
                auto tint = accentFor(2 + i);
                tint.setAlpha(55);
                fill.setColorAt(0, tint);
                fill.setColorAt(1, QColor(255, 255, 255, 10));
                p.fillPath(path, fill);
                p.setFont(uiFont(11, true));
                p.setPen(MUTED);
                p.drawText(tile.adjusted(14, 12, -14, 0),
                           Qt::AlignLeft | Qt::AlignTop,
                           tiles[i].label.toUpper());
                auto valueFont = uiFont(22, true);
                QFontMetricsF metrics(valueFont);
                double size = 22;
                while (metrics.horizontalAdvance(tiles[i].value) >
                           tile.width() - 28 &&
                       size > 12)
                {
                    size -= 1;
                    valueFont = uiFont(size, true);
                    metrics = QFontMetricsF(valueFont);
                }
                const bool emoteTile = i == 3 && !s.emotes.empty();
                if (emoteTile &&
                    this->drawEmote(p,
                                    QRectF(tile.left() + 14, tile.bottom() - 52,
                                           tile.width() - 28, 40),
                                    s.emotes.front().first, Qt::AlignLeft))
                {
                    continue;
                }
                p.setFont(valueFont);
                p.setPen(WHITE);
                p.drawText(tile.adjusted(14, 0, -14, -14),
                           Qt::AlignLeft | Qt::AlignBottom, tiles[i].value);
            }
            drawCentered(p,
                         QRectF(body.left(),
                                body.top() + 46 + (3 * (tileHeight + gap)) + 8,
                                body.width(), 22),
                         QStringLiteral("jilchat wrapped"), uiFont(12, true),
                         QColor(255, 120, 124, 200));
            break;
        }
    }
}

QPixmap WrappedDialog::renderCard(double scale) const
{
    QPixmap picture((QSizeF(CARD_WIDTH, CARD_HEIGHT) * scale).toSize());
    picture.setDevicePixelRatio(scale);
    picture.fill(Qt::transparent);
    QPainter p(&picture);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QRectF card(0, 0, CARD_WIDTH, CARD_HEIGHT);
    const auto slide = this->slides_.value(this->index_);

    QPainterPath shape;
    shape.addRoundedRect(card, RADIUS, RADIUS);
    QLinearGradient background(card.topLeft(), card.bottomLeft());
    background.setColorAt(0, QColor(30, 14, 18));
    background.setColorAt(1, QColor(12, 8, 10));
    p.fillPath(shape, background);

    p.save();
    p.setClipPath(shape);
    const auto accent = accentFor(int(slide));
    QRadialGradient glow(card.left() + 60, card.top() + 40, 360);
    glow.setColorAt(0,
                    QColor(accent.red(), accent.green(), accent.blue(), 120));
    glow.setColorAt(1, QColor(accent.red(), accent.green(), accent.blue(), 0));
    p.fillRect(card, glow);
    QRadialGradient glow2(card.right() - 20, card.bottom() - 60, 300);
    glow2.setColorAt(0,
                     QColor(accent.red(), accent.green(), accent.blue(), 55));
    glow2.setColorAt(1, QColor(accent.red(), accent.green(), accent.blue(), 0));
    p.fillRect(card, glow2);
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);

    this->paintSlide(p, card, slide);
    return picture;
}

void WrappedDialog::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto &card = this->card_;

    // Soft drop shadow
    p.setPen(Qt::NoPen);
    for (int i = SHADOW; i > 0; i -= 2)
    {
        p.setBrush(QColor(0, 0, 0, 9));
        p.drawRoundedRect(card.adjusted(-i, -i + 5, i, i + 5), RADIUS + i,
                          RADIUS + i);
    }

    p.drawPixmap(card.topLeft(), this->renderCard(this->devicePixelRatioF()));
    if (!this->fading_.isNull())
    {
        const double t =
            std::min(1.0, double(this->fadeClock_.elapsed()) / FADE_MS);
        p.setOpacity(1.0 - (t * t * (3 - (2 * t))));
        p.drawPixmap(card.topLeft(), this->fading_);
        p.setOpacity(1.0);
    }

    // Dots for how far along you are
    const int count = int(this->slides_.size());
    const double dotsWidth = (count * 8) + ((count - 1) * 6);
    double x = card.center().x() - (dotsWidth / 2);
    const double y = card.bottom() - PAD - 38 - 22;
    for (int i = 0; i < count; ++i)
    {
        p.setBrush(i == this->index_ ? QColor(255, 255, 255, 220)
                                     : QColor(255, 255, 255, 50));
        p.drawEllipse(QRectF(x, y, 8, 8));
        x += 14;
    }
    // Outlines below draw with no fill, not the last dot's color
    p.setBrush(Qt::NoBrush);

    for (int i = 0; i < this->controls_.size(); ++i)
    {
        const auto &control = this->controls_[i];
        const bool hover = i == this->hovered_;
        if (control.label == QStringLiteral("×"))
        {
            if (hover)
            {
                p.setBrush(QColor(255, 255, 255, 26));
                p.drawRoundedRect(control.rect, 8, 8);
            }
            p.setPen(
                QPen(hover ? WHITE : MUTED, 1.6, Qt::SolidLine, Qt::RoundCap));
            const auto c = control.rect.center();
            p.drawLine(c + QPointF(-5, -5), c + QPointF(5, 5));
            p.drawLine(c + QPointF(5, -5), c + QPointF(-5, 5));
            p.setPen(Qt::NoPen);
            continue;
        }
        QPainterPath path;
        path.addRoundedRect(control.rect, 10, 10);
        if (control.primary)
        {
            QLinearGradient fill(control.rect.topLeft(),
                                 control.rect.bottomLeft());
            fill.setColorAt(0,
                            hover ? QColor(255, 60, 70) : QColor(236, 34, 46));
            fill.setColorAt(1,
                            hover ? QColor(205, 24, 36) : QColor(178, 14, 26));
            p.fillPath(path, fill);
        }
        else if (control.selected)
        {
            p.fillPath(path, QColor(255, 255, 255, 46));
            p.setPen(QPen(QColor(255, 255, 255, 120), 1));
            p.drawPath(path);
        }
        else
        {
            p.fillPath(path, QColor(255, 255, 255, hover ? 30 : 14));
            p.setPen(QPen(QColor(255, 255, 255, hover ? 60 : 32), 1));
            p.drawPath(path);
        }
        p.setPen(control.primary || control.selected ? QColor(255, 255, 255)
                                                     : WHITE);
        p.setFont(uiFont(14, true));
        p.drawText(control.rect, Qt::AlignCenter, control.label);
        p.setPen(Qt::NoPen);
    }

    if (!this->toast_.isEmpty())
    {
        drawCentered(p,
                     QRectF(card.left(), card.bottom() - PAD - 38 - 50,
                            card.width(), 20),
                     this->toast_, uiFont(12, true), QColor(139, 227, 168));
    }
}

void WrappedDialog::saveImage()
{
    const auto folder =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const auto name = QStringLiteral("jilchat-wrapped-%1.png")
                          .arg(this->period_ == Period::ThisYear
                                   ? this->summary_.from.toString("yyyy")
                                   : this->summary_.from.toString("yyyy-MM"));
    const auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save your JilChat Wrapped"), folder + "/" + name,
        QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty())
    {
        return;
    }
    if (this->renderCard(2.0).save(path, "PNG"))
    {
        this->toast_ = QStringLiteral("Saved");
    }
    else
    {
        this->toast_ = QStringLiteral("Couldn't save the picture there");
    }
    this->update();
}

void WrappedDialog::copyImage()
{
    QGuiApplication::clipboard()->setPixmap(this->renderCard(2.0));
    this->toast_ = QStringLiteral("Copied. Paste it anywhere.");
    this->update();
}

void WrappedDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        this->move(event->globalPosition().toPoint() - this->dragOffset_);
        return;
    }
    int hovered = -1;
    for (int i = 0; i < this->controls_.size(); ++i)
    {
        if (this->controls_[i].rect.contains(event->position()))
        {
            hovered = i;
            break;
        }
    }
    if (hovered != this->hovered_)
    {
        this->hovered_ = hovered;
        this->setCursor(hovered >= 0 ? Qt::PointingHandCursor
                                     : Qt::ArrowCursor);
        this->update();
    }
}

void WrappedDialog::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    this->pressed_ = this->hovered_;
    if (this->pressed_ < 0 && this->card_.contains(event->position()))
    {
        this->dragging_ = true;
        this->dragOffset_ = event->globalPosition().toPoint() - this->pos();
    }
}

void WrappedDialog::mouseReleaseEvent(QMouseEvent *event)
{
    this->dragging_ = false;
    const int pressed = this->pressed_;
    this->pressed_ = -1;
    if (pressed >= 0 && pressed < this->controls_.size() &&
        this->controls_[pressed].rect.contains(event->position()))
    {
        // Copied first: the action may rebuild the controls
        const auto action = this->controls_[pressed].action;
        action();
    }
}

void WrappedDialog::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
        case Qt::Key_Right:
        case Qt::Key_Space:
        case Qt::Key_Return:
            this->goTo(this->index_ + 1);
            break;
        case Qt::Key_Left:
            this->goTo(this->index_ - 1);
            break;
        case Qt::Key_Escape:
            this->close();
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

void WrappedDialog::leaveEvent(QEvent * /*event*/)
{
    this->hovered_ = -1;
    this->setCursor(Qt::ArrowCursor);
    this->update();
}

// ------------------------------------------------------------------ Banner

namespace {

QPointer<WrappedBanner> shownBanner;

}  // namespace

void WrappedBanner::announce()
{
    auto *app = tryGetApp();
    if (app == nullptr || shownBanner)
    {
        return;
    }
    auto *container = app->getWindows()->getMainWindow().getLayoutContainer();
    auto *layout = container != nullptr
                       ? qobject_cast<QBoxLayout *>(container->layout())
                       : nullptr;
    if (layout == nullptr)
    {
        return;
    }
    shownBanner = new WrappedBanner(container);
    // Sits flush on top of the tabs, with no gap between
    layout->setSpacing(0);
    layout->insertWidget(0, shownBanner);
    shownBanner->show();
    transitions::fadeChange(container);
}

WrappedBanner::WrappedBanner(QWidget *parent)
    : QWidget(parent)
{
    this->setMouseTracking(true);
    this->setFixedHeight(42);
    this->setAttribute(Qt::WA_OpaquePaintEvent);
}

void WrappedBanner::layoutButtons()
{
    const double middle = this->height() / 2.0;
    this->close_ = QRectF(this->width() - 36, middle - 13, 26, 26);
    this->view_ = QRectF(this->close_.left() - 8 - 92, middle - 14, 92, 28);
}

void WrappedBanner::resizeEvent(QResizeEvent * /*event*/)
{
    this->layoutButtons();
}

void WrappedBanner::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF area(this->rect());

    QLinearGradient fill(area.topLeft(), area.topRight());
    fill.setColorAt(0, QColor(150, 14, 26));
    fill.setColorAt(0.6, QColor(110, 12, 30));
    fill.setColorAt(1, QColor(70, 14, 40));
    p.fillRect(area, fill);
    p.fillRect(QRectF(0, area.bottom() - 1, area.width(), 1),
               QColor(255, 90, 96, 140));

    // A little sparkle before the words
    const QPointF star(22, area.center().y());
    QPainterPath sparkle;
    sparkle.moveTo(star + QPointF(0, -8));
    sparkle.quadTo(star, star + QPointF(8, 0));
    sparkle.quadTo(star, star + QPointF(0, 8));
    sparkle.quadTo(star, star + QPointF(-8, 0));
    sparkle.quadTo(star, star + QPointF(0, -8));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 222, 140));
    p.drawPath(sparkle);

    const QRectF text(40, 0, this->view_.left() - 52, area.height());
    p.setFont(uiFont(14, true));
    p.setPen(QColor(255, 244, 242));
    const auto message = QStringLiteral(
        "Your monthly JilChat Wrapped is ready! Look in Settings to view it.");
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetricsF(p.font()).elidedText(message, Qt::ElideRight,
                                                  text.width()));

    QPainterPath button;
    button.addRoundedRect(this->view_, 8, 8);
    p.fillPath(button, this->hovered_ == 0 ? QColor(255, 255, 255, 235)
                                           : QColor(255, 255, 255, 205));
    p.setPen(QColor(140, 14, 26));
    p.setFont(uiFont(13, true));
    p.drawText(this->view_, Qt::AlignCenter, QStringLiteral("View"));

    if (this->hovered_ == 1)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawRoundedRect(this->close_, 6, 6);
    }
    p.setPen(QPen(QColor(255, 220, 218), 1.6, Qt::SolidLine, Qt::RoundCap));
    const auto c = this->close_.center();
    p.drawLine(c + QPointF(-4.5, -4.5), c + QPointF(4.5, 4.5));
    p.drawLine(c + QPointF(4.5, -4.5), c + QPointF(-4.5, 4.5));
}

void WrappedBanner::mouseMoveEvent(QMouseEvent *event)
{
    const auto at = event->position();
    const int hovered = this->view_.contains(at)    ? 0
                        : this->close_.contains(at) ? 1
                                                    : -1;
    if (hovered != this->hovered_)
    {
        this->hovered_ = hovered;
        this->setCursor(hovered >= 0 ? Qt::PointingHandCursor
                                     : Qt::ArrowCursor);
        this->update();
    }
}

void WrappedBanner::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    const auto at = event->position();
    if (!this->view_.contains(at) && !this->close_.contains(at))
    {
        return;
    }
    if (this->view_.contains(at))
    {
        WrappedDialog::showDialog(WrappedDialog::Period::LastMonth);
    }
    if (auto *parent = this->parentWidget())
    {
        transitions::fadeChange(parent);
    }
    this->hide();
    this->deleteLater();
}

void WrappedBanner::leaveEvent(QEvent * /*event*/)
{
    this->hovered_ = -1;
    this->setCursor(Qt::ArrowCursor);
    this->update();
}

}  // namespace chatterino
