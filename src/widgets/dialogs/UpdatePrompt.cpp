// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UpdatePrompt.hpp"

#include "Application.hpp"
#include "widgets/helper/JilChatCard.hpp"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIcon>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace chatterino {

namespace {

using namespace jilchatcard;

constexpr int CARD_WIDTH = 460;
constexpr int PAD = 24;
constexpr int LOGO = 58;
constexpr int BUTTON_HEIGHT = 36;
constexpr int NOTE_MAX_HEIGHT = 96;
/// The sectioned list can grow taller than a paragraph, but not past this
constexpr int CHANGES_MAX_HEIGHT = 380;
constexpr int BULLET_INDENT = 16;

/// The open window, so there's only ever one
QPointer<UpdatePrompt> openPrompt;
/// The version whose window was already shown or put off
QString handledVersion;

/// The sections, in the order they're shown, in shades of grey
struct Section {
    QString kind;
    QString label;
    QColor text;
    QColor fill;
    QColor border;
};

const std::array<Section, 4> &sections()
{
    static const std::array<Section, 4> list{{
        {"new", "NEW", QColor(20, 20, 22), QColor(240, 240, 242),
         QColor(255, 255, 255)},
        {"fixed", "FIXED", QColor(236, 236, 240), QColor(255, 255, 255, 34),
         QColor(255, 255, 255, 120)},
        {"changed", "CHANGED", QColor(200, 200, 206), QColor(255, 255, 255, 14),
         QColor(255, 255, 255, 70)},
        {"removed", "REMOVED", MUTED, QColor(255, 255, 255, 8),
         QColor(255, 255, 255, 40)},
    }};
    return list;
}

const Section &sectionFor(const QString &kind)
{
    for (const auto &section : sections())
    {
        if (section.kind == kind)
        {
            return section;
        }
    }
    return sections()[2];
}

double wrappedHeight(const QFont &font, int width, const QString &text)
{
    return std::ceil(
        QFontMetricsF(font)
            .boundingRect(QRectF(0, 0, width, 1000), Qt::TextWordWrap, text)
            .height());
}

double seconds(const QElapsedTimer &clock)
{
    return double(clock.elapsed()) / 1000.0;
}

}  // namespace

void UpdatePrompt::showPrompt()
{
    if (openPrompt == nullptr)
    {
        openPrompt = new UpdatePrompt();
    }
    handledVersion = getApp()->getUpdates().getOnlineVersion();
    openPrompt->show();
    openPrompt->raise();
}

void UpdatePrompt::showIfNew()
{
    const auto &updates = getApp()->getUpdates();
    if (updates.getStatus() != Updates::UpdateAvailable ||
        updates.getOnlineVersion().isEmpty() ||
        updates.getOnlineVersion() == handledVersion)
    {
        return;
    }
    UpdatePrompt::showPrompt();
}

UpdatePrompt::UpdatePrompt()
    : QWidget(nullptr,
              Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setAttribute(Qt::WA_ShowWithoutActivating);
    this->setAttribute(Qt::WA_QuitOnClose, false);
    this->setMouseTracking(true);
    this->setWindowTitle(QStringLiteral("JilChat Chatterino update"));
    this->setWindowIcon(QIcon(QStringLiteral(":/icon.ico")));

    this->changes_ = getApp()->getUpdates().getChanges();
    this->layOut();

    this->timer_.setTimerType(Qt::PreciseTimer);
    this->timer_.setInterval(16);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->tick();
    });
    this->timer_.start();
    this->clock_.start();

    if (auto *screen = QGuiApplication::primaryScreen())
    {
        const auto area = screen->availableGeometry();
        this->move(area.center() - this->rect().center());
    }

    this->connections_.managedConnect(getApp()->getUpdates().statusUpdated,
                                      [this](auto) {
                                          this->statusChanged();
                                      });
}

QString UpdatePrompt::message() const
{
    const auto &updates = getApp()->getUpdates();
    if (updates.isError())
    {
        return updates.problemText();
    }
    if (this->changes_.empty())
    {
        return QStringLiteral(
            "A new version is ready. Your chats and settings stay exactly as "
            "they are.");
    }
    return {};
}

bool UpdatePrompt::busy() const
{
    const auto status = getApp()->getUpdates().getStatus();
    return status == Updates::Downloading || status == Updates::Searching;
}

void UpdatePrompt::layOut()
{
    const int textWidth = CARD_WIDTH - PAD * 2;
    const auto text = this->message();
    this->showingMessage_ = !text.isEmpty();
    this->pieces_.clear();
    // Anything taller than the card's limit scrolls
    this->contentHeight_ =
        this->showingMessage_
            ? int(wrappedHeight(jilchatcard::font(13, false), textWidth, text))
            : this->layoutChanges(textWidth);
    const int noteHeight =
        std::min(this->showingMessage_ ? NOTE_MAX_HEIGHT : CHANGES_MAX_HEIGHT,
                 this->contentHeight_);
    const int header = PAD + LOGO;
    const int cardHeight =
        header + 16 + noteHeight + 8 + 18 + 18 + BUTTON_HEIGHT + PAD;

    const auto oldSize = this->size();
    this->setFixedSize(CARD_WIDTH + SHADOW * 2, cardHeight + SHADOW * 2);
    if (this->isVisible() && oldSize.height() != this->height())
    {
        // Grows and shrinks around its middle
        this->move(this->pos() +
                   QPoint(0, (oldSize.height() - this->height()) / 2));
    }
    this->card_ = QRectF(SHADOW, SHADOW, CARD_WIDTH, cardHeight);
    this->notesArea_ =
        QRectF(this->card_.left() + PAD, this->card_.top() + header + 16,
               textWidth, noteHeight);

    const double buttonsTop = this->card_.bottom() - PAD - BUTTON_HEIGHT;
    const double right = this->card_.right() - PAD;
    this->controls_[UpdateNow] =
        QRectF(right - 130, buttonsTop, 130, BUTTON_HEIGHT);
    this->controls_[RemindLater] =
        QRectF(right - 130 - 8 - 140, buttonsTop, 140, BUTTON_HEIGHT);
    this->controls_[Close] =
        QRectF(this->card_.right() - 38, this->card_.top() + 10, 28, 28);
    this->scrollTo(0, true);
}

void UpdatePrompt::statusChanged()
{
    const auto status = getApp()->getUpdates().getStatus();
    if (status == Updates::NoUpdateAvailable)
    {
        this->close();
        return;
    }
    this->layOut();
    this->update();
}

int UpdatePrompt::layoutChanges(int width)
{
    const auto titleFont = jilchatcard::font(13, true);
    const auto detailFont = jilchatcard::font(12, false);
    const auto tagFont = jilchatcard::font(10, true);
    const int textWidth = width - BULLET_INDENT;

    double y = 0;
    bool first = true;
    for (const auto &section : sections())
    {
        std::vector<const Updates::Change *> inSection;
        for (const auto &change : this->changes_)
        {
            if (&sectionFor(change.kind) == &section)
            {
                inSection.push_back(&change);
            }
        }
        if (inSection.empty())
        {
            continue;
        }

        if (!first)
        {
            y += 12;
        }
        first = false;

        const double tagWidth =
            QFontMetricsF(tagFont).horizontalAdvance(section.label) + 18;
        this->pieces_.push_back({Piece::Kind::Tag, QRectF(0, y, tagWidth, 19),
                                 section.label, section.kind});
        y += 19 + 8;

        for (const auto *change : inSection)
        {
            const double titleHeight =
                wrappedHeight(titleFont, textWidth, change->title);
            this->pieces_.push_back({Piece::Kind::Bullet,
                                     QRectF(3, y + 6, 6, 6),
                                     {},
                                     section.kind});
            this->pieces_.push_back(
                {Piece::Kind::Title,
                 QRectF(BULLET_INDENT, y, textWidth, titleHeight),
                 change->title, section.kind});
            y += titleHeight;

            if (!change->detail.isEmpty())
            {
                const double detailHeight =
                    wrappedHeight(detailFont, textWidth, change->detail);
                y += 1;
                this->pieces_.push_back(
                    {Piece::Kind::Detail,
                     QRectF(BULLET_INDENT, y, textWidth, detailHeight),
                     change->detail, section.kind});
                y += detailHeight;
            }
            y += 9;
        }
        y -= 9;
    }
    return int(std::ceil(y));
}

void UpdatePrompt::drawChanges(QPainter &p)
{
    const auto origin = this->notesArea_.topLeft();
    for (const auto &piece : this->pieces_)
    {
        const auto &section = sectionFor(piece.changeKind);
        const auto rect = piece.rect.translated(origin);
        switch (piece.kind)
        {
            case Piece::Kind::Tag:
                p.setPen(QPen(section.border, 1));
                p.setBrush(section.fill);
                p.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
                p.setFont(jilchatcard::font(10, true));
                p.setPen(section.text);
                p.drawText(rect, Qt::AlignCenter, piece.text);
                break;

            case Piece::Kind::Bullet:
                p.setPen(Qt::NoPen);
                p.setBrush(piece.changeKind == u"new" ? TEXT : MUTED);
                p.drawEllipse(rect);
                break;

            case Piece::Kind::Title:
                p.setFont(jilchatcard::font(13, true));
                p.setPen(TEXT);
                p.drawText(rect,
                           Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                           piece.text);
                break;

            case Piece::Kind::Detail:
                p.setFont(jilchatcard::font(12, false));
                p.setPen(MUTED);
                p.drawText(rect,
                           Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                           piece.text);
                break;
        }
    }
}

double UpdatePrompt::maxScroll() const
{
    return std::max(0.0, this->contentHeight_ - this->notesArea_.height());
}

void UpdatePrompt::scrollTo(double offset, bool instant)
{
    this->scrollTarget_ = std::clamp(offset, 0.0, this->maxScroll());
    if (instant)
    {
        this->scroll_ = this->scrollTarget_;
    }
    this->update();
}

QRectF UpdatePrompt::scrollTrack() const
{
    // In the card's right padding, beside the list
    return {this->notesArea_.right() + 9, this->notesArea_.top(), 4,
            this->notesArea_.height()};
}

QRectF UpdatePrompt::scrollThumb() const
{
    const auto track = this->scrollTrack();
    const double height = std::max(28.0, track.height() * track.height() /
                                             std::max(1, this->contentHeight_));
    const double travel = track.height() - height;
    const double max = this->maxScroll();
    return {track.left(),
            track.top() + (max > 0 ? travel * this->scroll_ / max : 0.0),
            track.width(), height};
}

void UpdatePrompt::drawNotes(QPainter &p)
{
    // Drawn on its own layer, so the edges can fade out where it scrolls
    const double dpr = this->devicePixelRatioF();
    QPixmap layer((this->notesArea_.size() * dpr).toSize());
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    {
        QPainter lp(&layer);
        lp.setRenderHints(p.renderHints());
        lp.translate(-this->notesArea_.topLeft());
        lp.translate(0, -this->scroll_);
        if (!this->showingMessage_)
        {
            this->drawChanges(lp);
        }
        else
        {
            lp.setFont(jilchatcard::font(13, false));
            lp.setPen(getApp()->getUpdates().isError() ? TEXT : MUTED);
            lp.drawText(
                QRectF(this->notesArea_.topLeft(),
                       QSizeF(this->notesArea_.width(),
                              std::max<double>(this->contentHeight_,
                                               this->notesArea_.height()))),
                Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                this->message());
        }

        const double height = this->notesArea_.height();
        const double fade = std::min(28.0, height / 4);
        const bool moreAbove = this->scroll_ > 0.5;
        const bool moreBelow = this->scroll_ < this->maxScroll() - 0.5;
        if (moreAbove || moreBelow)
        {
            lp.resetTransform();
            lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            QLinearGradient mask(0, 0, 0, height);
            mask.setColorAt(0, QColor(0, 0, 0, moreAbove ? 0 : 255));
            mask.setColorAt(fade / height, Qt::black);
            mask.setColorAt(1 - (fade / height), Qt::black);
            mask.setColorAt(1, QColor(0, 0, 0, moreBelow ? 0 : 255));
            lp.fillRect(QRectF(0, 0, this->notesArea_.width(), height), mask);
        }
    }
    p.drawPixmap(this->notesArea_.topLeft(), layer);
}

void UpdatePrompt::drawScrollBar(QPainter &p)
{
    if (this->maxScroll() <= 0)
    {
        return;
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 18));
    p.drawRoundedRect(this->scrollTrack(), 2, 2);
    const bool active = this->thumbHovered_ || this->draggingThumb_;
    p.setBrush(active ? QColor(255, 255, 255, 200) : QColor(255, 255, 255, 90));
    p.drawRoundedRect(this->scrollThumb(), 2, 2);
}

void UpdatePrompt::wheelEvent(QWheelEvent *event)
{
    if (this->maxScroll() <= 0)
    {
        event->ignore();
        return;
    }
    const auto pixels = event->pixelDelta();
    const double step = !pixels.isNull()
                            ? double(pixels.y())
                            : double(event->angleDelta().y()) / 120.0 * 52.0;
    this->scrollTo(this->scrollTarget_ - step, !pixels.isNull());
    event->accept();
}

void UpdatePrompt::tick()
{
    // Hover eases in and out; the glow behind the logo breathes slowly
    for (int i = 0; i < ControlCount; ++i)
    {
        const double target = this->hovered_ == i ? 1.0 : 0.0;
        this->hover_[i] += (target - this->hover_[i]) * 0.22;
    }
    this->scroll_ += (this->scrollTarget_ - this->scroll_) * 0.3;
    if (std::abs(this->scrollTarget_ - this->scroll_) < 0.25)
    {
        this->scroll_ = this->scrollTarget_;
    }
    this->update();
}

int UpdatePrompt::controlAt(const QPointF &position) const
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

void UpdatePrompt::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);

    const auto &updates = getApp()->getUpdates();
    const auto &card = this->card_;
    const double now = seconds(this->clock_);
    const QPointF logoCenter(card.left() + PAD + LOGO / 2.0,
                             card.top() + PAD + LOGO / 2.0);
    drawCard(p, card, now, logoCenter, 190);

    drawLogo(p, QRectF(card.left() + PAD, card.top() + PAD, LOGO, LOGO), now);

    // "NEW UPDATE" pill
    const double textLeft = card.left() + PAD + LOGO + 16;
    const QString pillText = updates.isError() ? QStringLiteral("UPDATE PAUSED")
                             : this->busy()    ? QStringLiteral("DOWNLOADING")
                                               : QStringLiteral("NEW UPDATE");
    const QRectF pill(
        textLeft, card.top() + PAD + 2,
        QFontMetricsF(jilchatcard::font(10, true)).horizontalAdvance(pillText) +
            18,
        19);
    drawPill(p, pill, pillText, TEXT, QColor(255, 255, 255, 22),
             QColor(255, 255, 255, 90));

    // Version
    p.setFont(jilchatcard::font(21, true));
    p.setPen(TEXT);
    const auto title = QFontMetricsF(p.font()).elidedText(
        QStringLiteral("JilChat Chatterino %1").arg(updates.getOnlineVersion()),
        Qt::ElideRight, card.right() - PAD - 30 - textLeft);
    p.drawText(QPointF(textLeft, card.top() + PAD + 48), title);

    // What's new: in sections when the release lists its changes, otherwise
    // one paragraph. A failed update shows why instead.
    this->drawNotes(p);
    this->drawScrollBar(p);

    p.setFont(jilchatcard::font(11, false));
    p.setPen(QColor(MUTED.red(), MUTED.green(), MUTED.blue(), 190));
    QString footer;
    if (this->busy())
    {
        footer = QStringLiteral(
            "Downloading now. JilChat Chatterino restarts by itself when it's "
            "done.");
    }
    else if (updates.installsItself())
    {
        footer = QStringLiteral("It installs by itself and restarts. Your "
                                "chats and settings stay.");
    }
    else
    {
        footer = QStringLiteral("Opens the download, to install it yourself.");
    }
    p.drawText(QRectF(card.left() + PAD, this->notesArea_.bottom() + 8,
                      card.width() - PAD * 2, 18),
               Qt::AlignLeft | Qt::AlignVCenter, footer);

    drawButton(p, this->controls_[RemindLater],
               QStringLiteral("Remind me later"), false,
               this->hover_[RemindLater], false, now);
    QString primary = QStringLiteral("Update now");
    if (this->busy())
    {
        primary = QStringLiteral("Downloading...");
    }
    else if (updates.isError())
    {
        primary = QStringLiteral("Try again");
    }
    else if (!updates.installsItself())
    {
        primary = QStringLiteral("Download");
    }
    drawButton(p, this->controls_[UpdateNow], primary, true,
               this->busy() ? 0.0 : this->hover_[UpdateNow], this->busy(), now);

    drawClose(p, this->controls_[Close], this->hover_[Close]);
}

void UpdatePrompt::mouseMoveEvent(QMouseEvent *event)
{
    if (this->draggingThumb_)
    {
        const auto track = this->scrollTrack();
        const double travel = track.height() - this->scrollThumb().height();
        if (travel > 0)
        {
            const double top =
                event->position().y() - this->thumbGrab_ - track.top();
            this->scrollTo(top / travel * this->maxScroll(), true);
        }
        return;
    }
    if (this->dragging_)
    {
        this->move(event->globalPosition().toPoint() - this->dragOffset_);
        return;
    }
    const bool overThumb =
        this->maxScroll() > 0 &&
        this->scrollTrack().adjusted(-6, 0, 6, 0).contains(event->position());
    if (overThumb != this->thumbHovered_)
    {
        this->thumbHovered_ = overThumb;
        this->update();
    }
    this->hovered_ = this->controlAt(event->position());
    this->setCursor(this->hovered_ >= 0 ? Qt::PointingHandCursor
                                        : Qt::ArrowCursor);
}

void UpdatePrompt::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    // The scrollbar: grab the thumb, or click the track to jump a page
    if (this->maxScroll() > 0 &&
        this->scrollTrack().adjusted(-6, 0, 6, 0).contains(event->position()))
    {
        const auto thumb = this->scrollThumb();
        if (thumb.adjusted(-6, 0, 6, 0).contains(event->position()))
        {
            this->draggingThumb_ = true;
            this->thumbGrab_ = event->position().y() - thumb.top();
        }
        else
        {
            const double page = this->notesArea_.height() * 0.85;
            this->scrollTo(
                this->scrollTarget_ +
                (event->position().y() < thumb.top() ? -page : page));
        }
        return;
    }
    this->pressed_ = this->controlAt(event->position());
    if (this->pressed_ < 0 && this->card_.contains(event->position()))
    {
        this->dragging_ = true;
        this->dragOffset_ = event->globalPosition().toPoint() - this->pos();
    }
}

void UpdatePrompt::mouseReleaseEvent(QMouseEvent *event)
{
    this->dragging_ = false;
    if (this->draggingThumb_)
    {
        this->draggingThumb_ = false;
        this->update();
        return;
    }
    const int released = this->controlAt(event->position());
    const int pressed = this->pressed_;
    this->pressed_ = -1;
    if (released < 0 || released != pressed)
    {
        return;
    }

    if (released == UpdateNow)
    {
        auto &updates = getApp()->getUpdates();
        if (this->busy() || !updates.canInstall())
        {
            return;
        }
        const bool opensDownload = !updates.installsItself();
        updates.installUpdates();
        if (opensDownload)
        {
            this->close();
        }
        return;
    }
    // "Remind me later" and the X: until the next start
    this->close();
}

void UpdatePrompt::leaveEvent(QEvent * /*event*/)
{
    this->hovered_ = -1;
    this->thumbHovered_ = false;
    this->setCursor(Qt::ArrowCursor);
}

}  // namespace chatterino
