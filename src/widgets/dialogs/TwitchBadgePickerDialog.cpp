// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchBadgePickerDialog.hpp"

#include "Application.hpp"
#include "common/Credentials.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/bttv/BttvUsernameEffects.hpp"
#include "providers/jilchat/JilChatBadges.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/Twitch.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/ColorButton.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/TooltipWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QtGlobal>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(332, 440);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;
constexpr int BADGE_GRID_SPACING = 4;
constexpr int BADGE_TILE_PADDING = 4;
constexpr QSize BADGE_ICON_SIZE(36, 36);
constexpr float BADGE_IMAGE_SCALE =
    float(BADGE_ICON_SIZE.width()) / float(BADGE_ICON_SIZE.width() / 2);
constexpr char BADGE_SELECTED_COLOR[] = "#9146ff";
constexpr auto MOLTORINO_BADGES_URL = "https://api.moltorino.com/v2/badges";
constexpr auto JILCHAT_ALL_BADGES_URL = "https://api.jil.chat/v1/badges";
constexpr auto JILCHAT_USER_BADGES_URL =
    "https://api.jil.chat/v1/badges/user/%1";
constexpr auto JILCHAT_ACTIVE_BADGE_URL =
    "https://api.jil.chat/v1/badges/me/active";
/// The slug of a milestone badge is this and the months it is for.
constexpr QStringView JILCHAT_MILESTONE_PREFIX = u"milestone_";
constexpr auto BTTV_API_URL = "https://api.betterttv.net/3/";
constexpr auto BTTV_BADGE_PATH = "account/subscription/badge";
constexpr auto BTTV_CREDENTIAL_PROVIDER = "bttv";
/// Run in the browser console on betterttv.com: copies the user's own
/// BetterTTV token.
constexpr auto BTTV_TOKEN_COMMAND =
    "(()=>{copy(JSON.parse(localStorage.getItem('USER_TOKEN')));"
    "return 'BetterTTV token copied.'})()";
constexpr auto BTTV_EFFECT_PATH = "account/subscription/username_effect";
constexpr auto SEVENTV_GQL_URL = "https://api.7tv.app/v4/gql";
/// Has the paint definitions in the format SeventvPaints reads.
constexpr auto SEVENTV_V3_GQL_URL = "https://7tv.io/v3/gql";
constexpr auto SEVENTV_PAINTS_QUERY =
    "query($list: [ObjectID!]) { cosmetics(list: $list) { paints { id name "
    "function color angle shape image_url repeat stops { at color } "
    "shadows { x_offset y_offset radius color } } } }";
constexpr int PAINT_TILE_HEIGHT = 50;
constexpr auto SEVENTV_CREDENTIAL_PROVIDER = "7tv";
/// Run in the browser console on 7tv.app: copies the user's own 7TV token.
constexpr auto SEVENTV_TOKEN_COMMAND =
    "(()=>{copy(localStorage.getItem('7tv-token'));"
    "return '7TV token copied.'})()";
constexpr auto SEVENTV_INVENTORY_QUERY = R"(query LeafyrinoVanityInventory {
  users {
    me {
      id
      connections { platform platformId platformDisplayName }
      style { activeBadgeId activePaintId }
      inventory(includeInaccessible: false) {
        badges { accessible to { badge { id name images { url width height scale } } } }
        paints { accessible to { paint { id name } } }
      }
    }
  }
})";
constexpr auto SEVENTV_SET_BADGE_MUTATION =
    R"(mutation SetActiveBadge($id: Id!, $badgeId: Id) {
  users { user(id: $id) { activeBadge(badgeId: $badgeId) { id } } }
})";
constexpr auto SEVENTV_SET_PAINT_MUTATION =
    R"(mutation SetActivePaint($id: Id!, $paintId: Id) {
  users { user(id: $id) { activePaint(paintId: $paintId) { id } } }
})";
constexpr auto MOLTORINO_PROFILE_URL = "https://api.moltorino.com/v2/badges/me";

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

int scaledMetric(float scale, int base, int minimum)
{
    return std::max(minimum, int(std::round(base * scale)));
}

int contentHorizontalMargin(float scale)
{
    return scaledMetric(scale, 12, 6);
}

int badgeTileSize()
{
    return BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2;
}

int badgeGridColumnsForWidth(int availableWidth)
{
    if (availableWidth <= 0)
    {
        const int fallbackWidth =
            DEFAULT_DIALOG_SIZE.width() - contentHorizontalMargin(1.0F) * 2;
        availableWidth = fallbackWidth;
    }

    const int stride = badgeTileSize() + BADGE_GRID_SPACING;
    return std::max(1, (availableWidth + BADGE_GRID_SPACING) / stride);
}

bool badgeMatchesSearch(const GqlBadge &badge, const QString &needle)
{
    if (needle.isEmpty())
    {
        return true;
    }

    return badge.title.contains(needle, Qt::CaseInsensitive) ||
           badge.setID.contains(needle, Qt::CaseInsensitive);
}

QLabel *makeEmptyListLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("TwitchBadgePickerEmpty");
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

ImageSet badgeImages(const GqlBadge &badge)
{
    const auto base = BADGE_ICON_SIZE / 2;

    ImagePtr image1;
    ImagePtr image2;
    ImagePtr image3;

    if (!badge.image1x.isEmpty())
    {
        image1 = Image::fromUrl(Url{badge.image1x}, 2, base);
    }
    if (!badge.image2x.isEmpty())
    {
        image2 = Image::fromUrl(Url{badge.image2x}, 1, BADGE_ICON_SIZE);
    }
    if (!badge.image4x.isEmpty())
    {
        image3 = Image::fromUrl(Url{badge.image4x}, 0.5, BADGE_ICON_SIZE * 2);
    }

    // Fall back to the best available resolution when a slot is missing.
    if (!image2 && image1)
    {
        image2 = image1;
    }
    if (!image3 && image2)
    {
        image3 = image2;
    }
    if (!image1 && image2)
    {
        image1 = image2;
    }
    if (!image2 && image3)
    {
        image2 = image3;
    }
    if (!image1 && image3)
    {
        image1 = image3;
    }

    const auto empty = getEmptyImagePtr();
    return ImageSet{
        image1 ? image1 : empty,
        image2 ? image2 : empty,
        image3 ? image3 : empty,
    };
}

void paintBadgeTileBackground(QPainter &painter, const QRect &rect,
                              bool underMouse, bool isDown)
{
    const auto *theme = getApp()->getThemes();
    auto bg = theme->splits.header.background;
    auto border = theme->splits.header.border;

    if (underMouse)
    {
        bg = theme->isLightTheme() ? bg.darker(105) : bg.lighter(115);
        border = theme->splits.header.focusedBorder;
    }
    if (isDown)
    {
        bg = theme->isLightTheme() ? bg.darker(112) : bg.lighter(125);
    }

    painter.setPen(QPen(border, 1));
    painter.setBrush(bg);
    painter.drawRoundedRect(rect, 3, 3);
}

void paintBadgeTileFocusRing(QPainter &painter, const QRect &rect)
{
    auto focus = getApp()->getThemes()->window.text;
    focus.setAlpha(200);
    painter.setPen(QPen(focus, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 3, 3);
}

class BadgePickerToggleSwitch final : public QPushButton
{
public:
    BadgePickerToggleSwitch(QWidget *parent)
        : QPushButton(parent)
    {
        this->setObjectName("TwitchBadgePickerToggleSwitch");
        this->setCheckable(true);
        this->setFlat(true);
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->updateMetrics(1.0F);
    }

    void updateMetrics(float scale)
    {
        this->setFixedSize(scaledMetric(scale, 38, 28),
                           scaledMetric(scale, 20, 14));
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto *theme = getApp()->getThemes();
        const auto track = this->rect().adjusted(0, 0, -1, -1);
        const int knobMargin = 2;
        const int knobDiameter = track.height() - knobMargin * 2;

        auto trackBg = theme->splits.input.background;
        auto trackBorder = theme->splits.header.border;

        if (this->isChecked())
        {
            trackBg = QColor("#9146ff");
            trackBorder = QColor("#9146ff");
        }
        else if (this->underMouse() && this->isEnabled())
        {
            trackBg = theme->isLightTheme() ? trackBg.darker(104)
                                            : trackBg.lighter(108);
            trackBorder = theme->splits.header.focusedBorder;
        }

        if (!this->isEnabled())
        {
            trackBg.setAlpha(120);
            trackBorder.setAlpha(120);
        }

        const qreal radius = track.height() / 2.0;
        painter.setPen(QPen(trackBorder, 1));
        painter.setBrush(trackBg);
        painter.drawRoundedRect(track, radius, radius);

        const int knobX = this->isChecked()
                              ? track.right() - knobMargin - knobDiameter + 1
                              : track.left() + knobMargin;
        const QRectF knobRect(knobX, track.top() + knobMargin, knobDiameter,
                              knobDiameter);

        auto knobColor = QColor("#ffffff");
        if (!this->isEnabled())
        {
            knobColor.setAlpha(180);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(knobColor);
        painter.drawEllipse(knobRect);

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, track);
        }
    }
};

QWidget *makeSettingRow(const QString &labelText,
                        BadgePickerToggleSwitch *toggle, QWidget *parent,
                        float scale)
{
    auto *row = new QWidget(parent);
    row->setObjectName("TwitchBadgePickerSettingRow");

    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(
        scaledMetric(scale, 8, 4), scaledMetric(scale, 6, 3),
        scaledMetric(scale, 8, 4), scaledMetric(scale, 6, 3));
    rowLayout->setSpacing(scaledMetric(scale, 8, 4));

    auto *label = new QLabel(labelText, row);
    label->setObjectName("TwitchBadgePickerSettingLabel");
    label->setWordWrap(true);
    label->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));

    rowLayout->addWidget(label, 1);
    rowLayout->addWidget(toggle, 0, Qt::AlignRight | Qt::AlignVCenter);

    return row;
}

/// The months a JilChat milestone badge is for; -1 for the other badges.
int jilChatMilestoneMonths(QStringView slug)
{
    if (!slug.startsWith(JILCHAT_MILESTONE_PREFIX))
    {
        return -1;
    }
    return slug.mid(JILCHAT_MILESTONE_PREFIX.size()).toInt();
}

/// The tooltip of the tile under the mouse. The pickers share it; it goes
/// away with the picker it was made for and is made again when needed.
QPointer<TooltipWidget> &tileTooltip()
{
    static QPointer<TooltipWidget> tooltip;
    return tooltip;
}

void showTileTooltip(QWidget *tile)
{
    const auto text = tile->toolTip();
    auto *window = dynamic_cast<BaseWidget *>(tile->window());
    if (text.isEmpty() || window == nullptr)
    {
        return;
    }
    auto &tooltip = tileTooltip();
    if (tooltip.isNull())
    {
        tooltip = new TooltipWidget(window);
    }
    tooltip->setOne(TooltipEntry{
        .image = nullptr,
        .text = text,
    });

    // Centered below the tile, wherever the mouse entered it; above the tile
    // if the screen ends below.
    const auto place = [&] {
        constexpr int gap = 4;
        const auto below =
            tile->mapToGlobal(QPoint(tile->width() / 2, tile->height()));
        int y = below.y() + gap;
        if (const auto *screen = QGuiApplication::screenAt(below);
            screen != nullptr &&
            y + tooltip->height() > screen->availableGeometry().bottom())
        {
            y = tile->mapToGlobal(QPoint(0, 0)).y() - gap - tooltip->height();
        }
        tooltip->moveTo({below.x() - (tooltip->width() / 2), y},
                        widgets::BoundsChecking::DesiredPosition);
    };
    place();
    tooltip->show();
    // Showing it can still change its size.
    place();
}

/// Shows a tile's tooltip as soon as the mouse is on it and hides it when
/// the mouse leaves, like the badges in the chat do. Qt's own tooltips come
/// and go with a delay. Returns whether the event is dealt with.
bool handleTileTooltip(QWidget *tile, QEvent *event)
{
    switch (event->type())
    {
        case QEvent::Enter:
            showTileTooltip(tile);
            return false;
        case QEvent::Leave:
        case QEvent::Hide:
            if (auto &tooltip = tileTooltip(); !tooltip.isNull())
            {
                tooltip->hide();
            }
            return false;
        case QEvent::ToolTip:
            // Qt would show its own tooltip now. Ours is there already,
            // unless the tile never heard of the mouse entering it.
            if (tileTooltip().isNull() || !tileTooltip()->isVisible())
            {
                showTileTooltip(tile);
            }
            return true;
        default:
            return false;
    }
}

class NoBadgeTileButton final : public QPushButton
{
public:
    NoBadgeTileButton(QWidget *parent)
        : QPushButton(parent)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip("No Badge");
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    bool event(QEvent *event) override
    {
        return handleTileTooltip(this, event) || QPushButton::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect iconRect(pad, pad, rect.width() - pad * 2,
                             rect.height() - pad * 2);
        const auto center = iconRect.center();
        const int radius =
            std::min(iconRect.width(), iconRect.height()) / 2 - 3;

        auto iconColor = getApp()->getThemes()->window.text;
        iconColor.setAlpha(220);
        painter.setPen(QPen(iconColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius, radius);

        const qreal diagonal = radius * 0.70710678118;
        painter.drawLine(QPointF(center.x() - diagonal, center.y() - diagonal),
                         QPointF(center.x() + diagonal, center.y() + diagonal));

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    bool selected_ = false;
};

class BadgeTileButton final : public QPushButton
{
public:
    BadgeTileButton(const GqlBadge &badge, QWidget *parent)
        : QPushButton(parent)
        , badge_(badge)
        , images_(badgeImages(badge))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip(badge.title);

        this->connections_.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                this->refreshImageIfNeeded();
            });
        // Animated badges only move when the tile is painted again.
        this->connections_.managedConnect(
            getApp()->getEmotes()->getGIFTimer()->signal, [this] {
                const auto &image =
                    this->images_.getImageOrLoaded(BADGE_IMAGE_SCALE);
                if (image->animated() && this->isVisible())
                {
                    this->update();
                }
            });
    }

    const GqlBadge &badge() const
    {
        return this->badge_;
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    bool event(QEvent *event) override
    {
        return handleTileTooltip(this, event) || QPushButton::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect imgRect(pad, pad, rect.width() - pad * 2,
                            rect.height() - pad * 2);

        const auto &image = this->images_.getImageOrLoaded(BADGE_IMAGE_SCALE);
        if (auto pixmap = image->pixmapOrLoad())
        {
            this->attemptRefresh_ = false;
            painter.drawPixmap(imgRect, *pixmap, pixmap->rect());
        }
        else
        {
            this->attemptRefresh_ = true;
            auto muted = getApp()->getThemes()->window.text;
            muted.setAlpha(50);
            painter.setBrush(muted);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(imgRect, 2, 2);
        }

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    void refreshImageIfNeeded()
    {
        if (!this->attemptRefresh_)
        {
            return;
        }

        const auto &image = this->images_.getImageOrLoaded(BADGE_IMAGE_SCALE);
        if (image->pixmapOrLoad())
        {
            this->attemptRefresh_ = false;
            this->update();
        }
    }

    GqlBadge badge_;
    ImageSet images_;
    bool selected_ = false;
    bool attemptRefresh_ = false;
    pajlada::Signals::SignalHolder connections_;
};

/// The user's name in a 7TV paint with the paint's name below, like in
/// Moltorino.
class PaintTileButton final : public QPushButton
{
public:
    PaintTileButton(QString paintId, QString paintName, QString userName,
                    const QColor &userColor, QWidget *parent)
        : QPushButton(parent)
        , paintId_(std::move(paintId))
        , paintName_(std::move(paintName))
        , userName_(std::move(userName))
        , userColor_(userColor)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedHeight(PAINT_TILE_HEIGHT);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        // Animated paints only move when the tile is painted again.
        this->connections_.managedConnect(
            getApp()->getEmotes()->getGIFTimer()->signal, [this] {
                const auto paint =
                    getApp()->getSeventvPaints()->getPaintById(this->paintId_);
                if (paint && paint->animated() && this->isVisible())
                {
                    this->update();
                }
            });
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            paintBadgeTileBackground(painter,
                                     selectionRect.adjusted(2, 2, -2, -2),
                                     this->underMouse(), this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        // The name, in the paint if there is one.
        auto nameFont = this->font();
        nameFont.setBold(true);
        nameFont.setPointSizeF(nameFont.pointSizeF() * 1.15);
        const QFontMetricsF nameMetrics(nameFont);
        const auto nameText = nameMetrics.elidedText(
            this->userName_, Qt::ElideRight, rect.width() - 8);
        const QSizeF nameSize(nameMetrics.horizontalAdvance(nameText) + 2,
                              nameMetrics.height());
        const QPointF namePos((rect.width() - nameSize.width()) / 2, 5);

        const auto paint =
            this->paintId_.isEmpty()
                ? nullptr
                : getApp()->getSeventvPaints()->getPaintById(this->paintId_);
        if (paint)
        {
            const auto pixmap =
                paint->getPixmap(nameText, nameFont, this->userColor_, nameSize,
                                 1.0F, float(this->devicePixelRatioF()));
            painter.drawPixmap(namePos, pixmap);
        }
        else
        {
            painter.setFont(nameFont);
            painter.setPen(this->userColor_);
            painter.drawText(QRectF(namePos, nameSize), Qt::AlignLeft,
                             nameText);
        }

        // The paint's name below.
        auto label = getApp()->getThemes()->window.text;
        label.setAlpha(180);
        painter.setFont(this->font());
        painter.setPen(label);
        const QRectF labelRect(
            4, rect.height() - QFontMetricsF(this->font()).height() - 5,
            rect.width() - 8, QFontMetricsF(this->font()).height());
        painter.drawText(labelRect, Qt::AlignHCenter,
                         QFontMetricsF(this->font())
                             .elidedText(this->paintName_, Qt::ElideRight,
                                         labelRect.width()));

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    QString paintId_;
    QString paintName_;
    QString userName_;
    QColor userColor_;
    bool selected_ = false;
    pajlada::Signals::SignalHolder connections_;
};

/// The user's name with a BetterTTV username effect and the effect's name
/// below, like the 7TV paints.
class BttvEffectTileButton final : public QPushButton
{
public:
    /// `effect` is null for no effect.
    BttvEffectTileButton(const BttvUsernameEffect *effect, QString userName,
                         const QColor &userColor, QWidget *parent)
        : QPushButton(parent)
        , effectId_(effect == nullptr ? QString{} : effect->id)
        , label_(effect == nullptr ? QStringLiteral("No effect")
                                   : effect->label)
        , userName_(std::move(userName))
        , userColor_(userColor)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedHeight(PAINT_TILE_HEIGHT);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        if (effect != nullptr && !effect->texture.isEmpty())
        {
            this->texture_ = Image::fromUrl(Url{effect->texture});
            this->outline_ = QColor(effect->outline);
        }

        // The effects move; the texture also has to be loaded first.
        this->connections_.managedConnect(
            getApp()->getEmotes()->getGIFTimer()->signal, [this] {
                if (!this->isVisible())
                {
                    return;
                }
                if (this->texture_ != nullptr)
                {
                    // The texture moves by a fraction of a pixel per frame;
                    // a new picture every half pixel looks the same.
                    const auto step = int(textureOffset() * 2);
                    if (step != this->paintedStep_)
                    {
                        this->update();
                    }
                }
                else if (this->effectId_ == u"flare")
                {
                    // The light is only there half of the time.
                    const bool lit = flarePhase() < 1.0;
                    if (lit || this->flareLit_)
                    {
                        this->flareLit_ = lit;
                        this->update();
                    }
                }
            });
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

    /// Draws the tile dimmed. An opacity effect on the widget would do the
    /// same, but draws every frame of the moving name twice.
    void setLocked(bool locked)
    {
        this->locked_ = locked;
        this->update();
    }

protected:
    bool event(QEvent *event) override
    {
        return handleTileTooltip(this, event) || QPushButton::event(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (this->locked_)
        {
            painter.setOpacity(0.35);
        }

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            paintBadgeTileBackground(painter,
                                     selectionRect.adjusted(2, 2, -2, -2),
                                     this->underMouse(), this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        auto nameFont = this->font();
        nameFont.setBold(true);
        nameFont.setPointSizeF(nameFont.pointSizeF() * 1.15);
        const QFontMetricsF nameMetrics(nameFont);
        const auto nameText = nameMetrics.elidedText(
            this->userName_, Qt::ElideRight, rect.width() - 12);
        const auto nameWidth = nameMetrics.horizontalAdvance(nameText);
        const QPointF namePos((rect.width() - nameWidth) / 2, 5);
        QPainterPath name;
        name.addText(namePos.x(), namePos.y() + nameMetrics.ascent(), nameFont,
                     nameText);

        painter.setPen(Qt::NoPen);
        if (this->texture_ != nullptr)
        {
            // An outline that is thicker below, then the moving texture.
            painter.setPen(QPen(this->outline_, 2, Qt::SolidLine, Qt::RoundCap,
                                Qt::RoundJoin));
            painter.setBrush(this->outline_);
            painter.drawPath(name.translated(0, 2));
            painter.drawPath(name);
            painter.setPen(Qt::NoPen);

            const auto pixmap = this->texture_->pixmapOrLoad();
            if (pixmap)
            {
                this->paintedStep_ = int(textureOffset() * 2);
                const auto offset = this->paintedStep_ / 2.0;
                QBrush brush(*pixmap);
                brush.setTransform(QTransform::fromTranslate(-offset, -offset));
                painter.setBrush(brush);
            }
            else
            {
                painter.setBrush(this->userColor_);
            }
            painter.drawPath(name);
        }
        else if (this->effectId_ == u"glow")
        {
            // A soft light around the name in its color.
            auto light = this->userColor_;
            light.setAlpha(22);
            painter.setBrush(Qt::NoBrush);
            for (const int width : {12, 9, 6, 3})
            {
                painter.setPen(QPen(light, width, Qt::SolidLine, Qt::RoundCap,
                                    Qt::RoundJoin));
                painter.drawPath(name);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(this->userColor_);
            painter.drawPath(name);
        }
        else
        {
            painter.setBrush(this->userColor_);
            painter.drawPath(name);
            if (this->effectId_ == u"flare")
            {
                // A light that crosses the name in the first half of every
                // eight seconds.
                const auto phase = flarePhase();
                if (phase < 1.0)
                {
                    const auto center =
                        namePos.x() - nameWidth + (4 * nameWidth * phase);
                    const auto reach = 0.2 * nameWidth;
                    QColor shine((this->userColor_.red() + (4 * 255)) / 5,
                                 (this->userColor_.green() + (4 * 255)) / 5,
                                 (this->userColor_.blue() + (4 * 255)) / 5);
                    auto clear = shine;
                    clear.setAlpha(0);
                    QLinearGradient gradient(center - reach, 0, center + reach,
                                             0);
                    gradient.setColorAt(0, clear);
                    gradient.setColorAt(0.5, shine);
                    gradient.setColorAt(1, clear);
                    painter.setBrush(gradient);
                    painter.drawPath(name);
                }
            }
        }

        // The effect's name below.
        auto label = getApp()->getThemes()->window.text;
        label.setAlpha(180);
        painter.setFont(this->font());
        painter.setPen(label);
        const QRectF labelRect(
            4, rect.height() - QFontMetricsF(this->font()).height() - 5,
            rect.width() - 8, QFontMetricsF(this->font()).height());
        painter.drawText(
            labelRect, Qt::AlignHCenter,
            QFontMetricsF(this->font())
                .elidedText(this->label_, Qt::ElideRight, labelRect.width()));

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    /// Below 1 while the light of the flare crosses the name: the first
    /// half of every eight seconds.
    static double flarePhase()
    {
        return double(QDateTime::currentMSecsSinceEpoch() % 8000) / 4000.0;
    }

    /// How far the 96 pixel texture has moved; once around in 16 seconds.
    static double textureOffset()
    {
        return double(QDateTime::currentMSecsSinceEpoch() % 16000) * 96.0 /
               16000.0;
    }

    QString effectId_;
    QString label_;
    QString userName_;
    QColor userColor_;
    ImagePtr texture_;
    QColor outline_;
    /// The texture's position at the last paint, in half pixels.
    int paintedStep_ = -1;
    bool flareLit_ = false;
    bool locked_ = false;
    bool selected_ = false;
    pajlada::Signals::SignalHolder connections_;
};

class ColorTileButton final : public QPushButton
{
public:
    ColorTileButton(const QColor &color, const QString &label, QWidget *parent)
        : QPushButton(parent)
        , color_(color)
        , label_(label)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip(label);
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    bool event(QEvent *event) override
    {
        return handleTileTooltip(this, event) || QPushButton::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect colorRect(pad, pad, rect.width() - pad * 2,
                              rect.height() - pad * 2);

        painter.setPen(QPen(getApp()->getThemes()->splits.header.border, 1));
        painter.setBrush(this->color_);
        painter.drawRoundedRect(colorRect, 2, 2);

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    QColor color_;
    QString label_;
    bool selected_ = false;
};

bool isValidCustomHexColor(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re.match(text.trimmed()).hasMatch();
}

QString normalizeCustomHexInput(const QString &text)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#'))
    {
        return trimmed;
    }

    static const QRegularExpression hexOnly(
        QStringLiteral("^[0-9A-Fa-f]{1,6}$"));
    if (hexOnly.match(trimmed).hasMatch())
    {
        return '#' + trimmed;
    }

    return trimmed;
}

std::vector<BadgePreviewFallback> toBadgePreviewFallbacks(
    const std::unordered_map<QString, GqlBadge> &lookup)
{
    std::vector<BadgePreviewFallback> result;
    result.reserve(lookup.size());

    for (const auto &[setID, badge] : lookup)
    {
        std::ignore = setID;
        result.push_back({
            .setID = badge.setID,
            .version = badge.version,
            .title = badge.title,
            .image1x = badge.image1x,
            .image2x = badge.image2x,
            .image4x = badge.image4x,
        });
    }

    return result;
}

const GqlBadge *findAuthorityBadge(const QVector<GqlBadge> &badges,
                                   const QString &setID)
{
    for (const auto &badge : badges)
    {
        if (badge.setID.compare(setID, Qt::CaseInsensitive) == 0)
        {
            return &badge;
        }
    }

    return nullptr;
}

bool isLeadModeratorSet(const QString &setID)
{
    return setID.compare(QStringLiteral("lead_moderator"),
                         Qt::CaseInsensitive) == 0;
}

bool hasLeadModeratorChoice(const GqlChatSettingsBadges &badgeState)
{
    return findAuthorityBadge(badgeState.authorityBadges,
                              QStringLiteral("lead_moderator")) != nullptr &&
           findAuthorityBadge(badgeState.authorityBadges,
                              QStringLiteral("moderator")) != nullptr;
}

bool usesLeadModeratorBadge(const GqlChatSettingsBadges &badgeState)
{
    if (!badgeState.selectedAuthorityBadge.setID.isEmpty())
    {
        return isLeadModeratorSet(badgeState.selectedAuthorityBadge.setID);
    }

    // Twitch defaults lead mods to the hammer badge.
    return findAuthorityBadge(badgeState.authorityBadges,
                              QStringLiteral("lead_moderator")) != nullptr;
}

std::vector<TwitchBadge> collectPreviewBadges(
    const GqlChatSettingsBadges &badgeState, TwitchChannel *channel,
    const QString &userId)
{
    std::vector<TwitchBadge> badges;
    QSet<QString> addedSetIds;

    const auto addBadge = [&](const QString &setID, const QString &version) {
        if (setID.isEmpty() || addedSetIds.contains(setID))
        {
            return;
        }
        addedSetIds.insert(setID);
        badges.emplace_back(setID, version);
    };

    if (!badgeState.selectedAuthorityBadge.setID.isEmpty())
    {
        addBadge(badgeState.selectedAuthorityBadge.setID,
                 badgeState.selectedAuthorityBadge.version);
    }
    else
    {
        const auto *leadBadge = findAuthorityBadge(
            badgeState.authorityBadges, QStringLiteral("lead_moderator"));
        if (leadBadge != nullptr)
        {
            addBadge(leadBadge->setID, leadBadge->version);
        }
        else
        {
            for (const auto &badge : badgeState.authorityBadges)
            {
                addBadge(badge.setID, badge.version);
            }
        }
    }

    if (channel != nullptr)
    {
        const auto snapshot = channel->getMessageSnapshot(100);
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        {
            if ((*it)->userID != userId)
            {
                continue;
            }

            for (const auto &tb : (*it)->twitchBadges)
            {
                if (tb.flag_ == MessageElementFlag::BadgeSubscription)
                {
                    addBadge(tb.key_, tb.value_);
                }
            }
            break;
        }
    }

    // Custom channel badges replace the global vanity badge, not the sub badge.
    if (badgeState.useCustomChannelBadge)
    {
        addBadge(badgeState.selectedChannelBadge.setID,
                 badgeState.selectedChannelBadge.version);
    }
    else
    {
        addBadge(badgeState.selectedGlobalBadge.setID,
                 badgeState.selectedGlobalBadge.version);
    }

    return badges;
}

std::unordered_map<QString, GqlBadge> gqlBadgeLookup(
    const GqlChatSettingsBadges &badgeState)
{
    std::unordered_map<QString, GqlBadge> lookup;

    const auto add = [&](const GqlBadge &badge) {
        if (!badge.setID.isEmpty())
        {
            lookup[badge.setID] = badge;
        }
    };

    for (const auto &badge : badgeState.authorityBadges)
    {
        add(badge);
    }
    add(badgeState.selectedAuthorityBadge);
    add(badgeState.selectedGlobalBadge);
    add(badgeState.selectedChannelBadge);

    for (const auto &badge : badgeState.availableGlobal)
    {
        add(badge);
    }
    for (const auto &badge : badgeState.availableChannel)
    {
        add(badge);
    }

    return lookup;
}

QString resolveSelfDisplayName(const QString &userId, const QString &loginName,
                               TwitchChannel *channel)
{
    if (auto twitchUser = getApp()->getTwitchUsers()->resolveID({userId});
        twitchUser && !twitchUser->displayName.isEmpty())
    {
        return twitchUser->displayName;
    }

    if (channel != nullptr)
    {
        const auto snapshot = channel->getMessageSnapshot(100);
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        {
            if ((*it)->userID != userId)
            {
                continue;
            }

            if (!(*it)->displayName.isEmpty())
            {
                return (*it)->displayName;
            }

            if (!(*it)->localizedName.isEmpty())
            {
                return (*it)->localizedName;
            }
            break;
        }
    }

    return loginName;
}

int contentHorizontalChrome(float scale)
{
    return contentHorizontalMargin(scale) * 2 + scaledMetric(scale, 12, 8);
}

int measureMessageContentWidth(const MessagePtr &message, float scale,
                               float dpr)
{
    if (message == nullptr)
    {
        return 0;
    }

    MessageLayout layout(message);
    MessageColors colors;
    colors.applyTheme(getApp()->getThemes(), false, 255);

    constexpr int PROBE_WIDTH = 10000;
    layout.layout(
        {
            .messageColors = colors,
            .flags = getApp()->getWindows()->getWordFlags(),
            .width = PROBE_WIDTH,
            .scale = scale,
            .imageScale = scale * dpr,
            .selectedChannel = nullptr,
            .message = *message,
        },
        false);

    return layout.getLayoutContentWidth();
}

}  // namespace

std::vector<QPointer<TwitchBadgePickerDialog>>
    TwitchBadgePickerDialog::activeDialogs_;

TwitchBadgePickerDialog::TwitchBadgePickerDialog(TwitchChannel *channel,
                                                 QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("TwitchBadgePickerDialog");
    this->setWindowTitle("Select Badge");
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    container->setObjectName("TwitchBadgePickerDialogRoot");
    container->setMouseTracking(true);
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);

    // Header
    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("TwitchBadgePickerHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);

    this->headerTitleLabel_ = new QLabel("Select Badge", this->headerWidget_);
    this->headerTitleLabel_->setObjectName("TwitchBadgePickerTitle");
    headerLayout->addWidget(this->headerTitleLabel_);
    headerLayout->addStretch(1);

    this->pinButton_ = this->createPinButton();
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {.dark = ":/buttons/cancel.svg", .light = ":/buttons/cancelDark.svg"},
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    this->closeButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("TwitchBadgePickerDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    this->previewWidget_ = new QWidget(container);
    this->previewWidget_->setObjectName("TwitchBadgePickerPreview");
    auto *previewLayout = new QVBoxLayout(this->previewWidget_);
    previewLayout->setContentsMargins(0, scaledMetric(this->scale(), 4, 2), 0,
                                      scaledMetric(this->scale(), 4, 2));
    previewLayout->setSpacing(0);
    this->previewView_ = new MessageView(this->previewWidget_);
    previewLayout->addWidget(this->previewView_);
    this->mainLayout_->addWidget(this->previewWidget_);

    // Tab rows
    const int tabSpacing = 4;
    const int tabTopMargin = scaledMetric(this->scale(), 4, 2);
    auto *tabRows = new QVBoxLayout();
    tabRows->setSpacing(tabSpacing);
    tabRows->setContentsMargins(0, tabTopMargin, 0, 0);

    auto *badgeTabRow = new QHBoxLayout();
    badgeTabRow->setSpacing(tabSpacing);

    this->globalTabButton_ = new QPushButton("Global Badge", container);
    this->globalTabButton_->setObjectName("TwitchBadgePickerTab");
    this->globalTabButton_->setCheckable(true);
    this->globalTabButton_->setChecked(true);
    this->globalTabButton_->setCursor(Qt::PointingHandCursor);
    this->globalTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::Fixed);
    QObject::connect(this->globalTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::GlobalBadges);
                     });

    this->channelTabButton_ = new QPushButton("Channel Badge", container);
    this->channelTabButton_->setObjectName("TwitchBadgePickerTab");
    this->channelTabButton_->setCheckable(true);
    this->channelTabButton_->setChecked(false);
    this->channelTabButton_->setCursor(Qt::PointingHandCursor);
    this->channelTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Fixed);
    QObject::connect(this->channelTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::ChannelBadges);
                     });

    badgeTabRow->addWidget(this->globalTabButton_, 1);
    badgeTabRow->addWidget(this->channelTabButton_, 1);
    tabRows->addLayout(badgeTabRow);

    auto *otherTabRow = new QHBoxLayout();
    otherTabRow->setSpacing(tabSpacing);

    this->colorTabButton_ = new QPushButton("Color", container);
    this->colorTabButton_->setObjectName("TwitchBadgePickerTab");
    this->colorTabButton_->setCheckable(true);
    this->colorTabButton_->setChecked(false);
    this->colorTabButton_->setCursor(Qt::PointingHandCursor);
    this->colorTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);
    QObject::connect(this->colorTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::Color);
                     });

    this->eventTabButton_ = new QPushButton("Missing / Soon", container);
    this->eventTabButton_->setObjectName("TwitchBadgePickerTab");
    this->eventTabButton_->setCheckable(true);
    this->eventTabButton_->setChecked(false);
    this->eventTabButton_->setCursor(Qt::PointingHandCursor);
    this->eventTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);
    QObject::connect(this->eventTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::EventBadges);
                     });

    this->moltorinoTabButton_ = new QPushButton("Moltorino", container);
    this->moltorinoTabButton_->setObjectName("TwitchBadgePickerTab");
    this->moltorinoTabButton_->setCheckable(true);
    this->moltorinoTabButton_->setChecked(false);
    this->moltorinoTabButton_->setCursor(Qt::PointingHandCursor);
    this->moltorinoTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Fixed);
    QObject::connect(this->moltorinoTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::Moltorino);
                     });

    // Roughly in the order the badges have in the chat, each paint before
    // the badge of the same service; the rest comes last.
    tabRows->addLayout(otherTabRow);

    auto *extraTabRow = new QHBoxLayout();
    extraTabRow->setSpacing(tabSpacing);
    const auto makeTab = [&](QHBoxLayout *tabRow, const QString &text,
                             View view) {
        auto *button = new QPushButton(text, container);
        button->setObjectName("TwitchBadgePickerTab");
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(button, &QPushButton::clicked, this, [this, view] {
            this->switchView(view);
        });
        tabRow->addWidget(button, 1);
        return button;
    };
    this->bttvEffectTabButton_ =
        makeTab(otherTabRow, "BTTV Paint", View::BttvEffects);
    this->bttvTabButton_ = makeTab(otherTabRow, "BTTV Badge", View::Bttv);
    otherTabRow->addWidget(this->moltorinoTabButton_, 1);
    auto *paintTabRow = new QHBoxLayout();
    paintTabRow->setSpacing(tabSpacing);
    this->bluzyrinoTabButton_ =
        makeTab(paintTabRow, "Bluzyrino", View::Bluzyrino);
    this->jilChatTabButton_ = makeTab(paintTabRow, "JilChat", View::JilChat);
    this->sevenTvPaintTabButton_ =
        makeTab(paintTabRow, "7TV Paint", View::SevenTvPaints);
    tabRows->addLayout(paintTabRow);
    this->sevenTvBadgeTabButton_ =
        makeTab(extraTabRow, "7TV Badge", View::SevenTvBadges);
    extraTabRow->addWidget(this->colorTabButton_, 1);
    extraTabRow->addWidget(this->eventTabButton_, 1);
    tabRows->addLayout(extraTabRow);
    this->mainLayout_->addLayout(tabRows);

    this->searchInput_ = new QLineEdit(container);
    this->searchInput_->setObjectName("TwitchBadgePickerSearch");
    this->searchInput_->setPlaceholderText("Search...");
    this->searchInput_->setClearButtonEnabled(true);
    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         this->searchQuery_ = text;
                         this->rebuildContent();
                         if (this->searchInput_ != nullptr)
                         {
                             this->searchInput_->setFocus(Qt::OtherFocusReason);
                             this->searchInput_->setCursorPosition(
                                 this->searchQuery_.size());
                         }
                     });
    this->searchRowWidget_ = new QWidget(container);
    auto *searchRow = new QHBoxLayout(this->searchRowWidget_);
    searchRow->setContentsMargins(0, scaledMetric(this->scale(), 4, 2), 0,
                                  scaledMetric(this->scale(), 4, 2));
    searchRow->addWidget(this->searchInput_);
    this->mainLayout_->addWidget(this->searchRowWidget_);

    // Scroll area
    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("TwitchBadgePickerScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->viewport()->setAutoFillBackground(false);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget();
    this->contentWidget_->setObjectName("TwitchBadgePickerDialogContent");
    this->contentWidget_->setMinimumWidth(0);
    this->contentWidget_->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(
        0, scaledMetric(this->scale(), 7, 4), 0,
        scaledMetric(this->scale(), 8, 4));
    this->contentLayout_->setSpacing(scaledMetric(this->scale(), 7, 4));
    this->scrollArea_->setWidget(this->contentWidget_);

    // The preview shows the badges of other providers too; they change when
    // one is picked here.
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->badgesUpdated, [this](const QString &) {
            // The Bluzyrino tab shows what
            // the registry lists.
            if (this->view_ == View::Bluzyrino && !this->actionInFlight_)
            {
                this->rebuildContent();
                return;
            }
            this->updatePreview();
        });

    this->signalHolder_.managedConnect(
        BttvUsernameEffects::instance().effectsUpdated, [this] {
            if (this->view_ == View::BttvEffects && !this->actionInFlight_)
            {
                this->rebuildContent();
            }
        });

    this->refreshStyle();
    this->rebuildContent();
}

void TwitchBadgePickerDialog::showDialog(TwitchChannel *channel,
                                         QWidget *parent)
{
    if (!channel)
        return;

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channel_ == channel)
        {
            (*it)->raise();
            (*it)->activateWindow();
            (*it)->loadBadges(true);
            return;
        }
        ++it;
    }

    auto *dialog = new TwitchBadgePickerDialog(channel, parent);
    activeDialogs_.push_back(dialog);

    QPoint center = QCursor::pos();
    if (parent && parent->window())
        center = parent->window()->geometry().center();

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->loadBadges(false);
}

void TwitchBadgePickerDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void TwitchBadgePickerDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
    this->updatePreview();
    if (this->badgeGridColumns() != this->lastBadgeGridColumns_)
    {
        this->rebuildContent();
    }
}

int TwitchBadgePickerDialog::badgeGridColumns() const
{
    int availableWidth = 0;
    if (this->scrollArea_ != nullptr)
    {
        availableWidth = this->scrollArea_->viewport()->width();
    }

    if (availableWidth <= 0)
    {
        const float scale = this->scale();
        availableWidth =
            int(std::round(float(this->scaleIndependentWidth()) * scale)) -
            contentHorizontalMargin(scale) * 2;
    }

    return badgeGridColumnsForWidth(availableWidth);
}

void TwitchBadgePickerDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);

    const int minW = this->minimumWidth();
    const int minH = this->minimumHeight();
    if (this->width() < minW || this->height() < minH)
    {
        this->resize(std::max(this->width(), minW),
                     std::max(this->height(), minH));
    }

    if (this->badgeGridColumns() != this->lastBadgeGridColumns_)
    {
        this->rebuildContent();
    }

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->horizontalScrollBar()->setValue(0);
    }
}

void TwitchBadgePickerDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    if (!this->initialFetchDone_)
    {
        QTimer::singleShot(0, this, [this] {
            if (!this->initialFetchDone_)
                this->loadBadges(false);
        });
    }
}

void TwitchBadgePickerDialog::loadBadges(bool force)
{
    if (this->badgesLoading_ && !force)
        return;

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        this->initialFetchDone_ = true;
        this->rebuildContent();
        return;
    }

    this->initialFetchDone_ = true;
    this->badgesLoading_ = true;
    this->setStatus("Loading badges...");
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelLogin = this->channel_->getName();

    TwitchGql::getChatSettingsBadges(
        channelLogin, token,
        [self](GqlChatSettingsBadges badges) {
            if (!self)
                return;
            self->badgesLoading_ = false;
            self->badgesLoaded_ = true;
            self->badges_ = std::move(badges);
            self->setStatus({});
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->badgesLoading_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("loading badges", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::rebuildContent()
{
    this->clearContent();

    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("TwitchBadgePickerStatus");
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->setAlignment(Qt::AlignCenter);
    this->statusLabel_->hide();
    this->contentLayout_->addWidget(this->statusLabel_);
    this->setStatus(this->statusText_, this->statusIsError_);

    if (this->badgesLoading_ && this->view_ != View::Color &&
        this->view_ != View::Bttv && this->view_ != View::BttvEffects &&
        this->view_ != View::Moltorino && this->view_ != View::JilChat &&
        this->view_ != View::Bluzyrino && this->view_ != View::SevenTvBadges &&
        this->view_ != View::SevenTvPaints)
    {
        this->setStatus("Loading badges...");
        this->contentLayout_->addStretch(1);
        return;
    }

    if (this->view_ == View::GlobalBadges)
    {
        this->rebuildGlobalBadges();
    }
    else if (this->view_ == View::ChannelBadges)
    {
        this->rebuildChannelBadges();
    }
    else if (this->view_ == View::EventBadges)
    {
        this->rebuildEventBadges();
    }
    else if (this->view_ == View::Moltorino)
    {
        this->rebuildMoltorinoBadges();
    }
    else if (this->view_ == View::JilChat)
    {
        this->rebuildJilChatBadges();
    }
    else if (this->view_ == View::Bluzyrino)
    {
        this->rebuildBluzyrinoBadges();
    }
    else if (this->view_ == View::Bttv || this->view_ == View::BttvEffects)
    {
        this->rebuildBttv();
    }
    else if (this->view_ == View::SevenTvBadges ||
             this->view_ == View::SevenTvPaints)
    {
        this->rebuildSevenTv();
    }
    else
    {
        this->rebuildColors();
    }

    this->contentLayout_->addStretch(1);
    this->lastBadgeGridColumns_ = this->badgeGridColumns();
    this->applySizeConstraints();
    this->updatePreview();

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->horizontalScrollBar()->setValue(0);
    }
}

void TwitchBadgePickerDialog::rebuildGlobalBadges()
{
    const auto &available = this->badges_.availableGlobal;
    const auto &selected = this->badges_.selectedGlobalBadge;
    const auto needle = this->searchQuery_.trimmed();

    if (available.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
            this->setStatus("No global badges available.");
        return;
    }

    int badgeCount = 0;
    for (const auto &badge : available)
    {
        if (badgeMatchesSearch(badge, needle))
        {
            ++badgeCount;
        }
    }

    const auto labelText =
        badgeCount > 0 ? QStringLiteral("Your Badges (%1)").arg(badgeCount)
                       : QStringLiteral("Your Badges");
    auto *label = new QLabel(labelText, this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    const int gridColumns = this->badgeGridColumns();

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;

    if (needle.isEmpty())
    {
        auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
        noBadgeTile->setEnabled(!this->actionInFlight_);
        if (selected.setID.isEmpty())
            noBadgeTile->setSelected(true);
        QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
            this->selectGlobal({});
        });
        grid->addWidget(noBadgeTile, row, col);
        col = 1;
    }

    int shown = 0;
    for (const auto &badge : available)
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version)
            tile->setSelected(true);

        QObject::connect(tile, &QPushButton::clicked, this, [this, badge] {
            this->selectGlobal(badge);
        });
        grid->addWidget(tile, row, col);
        ++shown;
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    }

    if (shown == 0 && !needle.isEmpty())
    {
        if (!this->statusIsError_)
        {
            this->setStatus({});
        }
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("No matching badges."), this->contentWidget_));
        return;
    }

    if (!this->statusIsError_)
    {
        this->setStatus({});
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::rebuildChannelBadges()
{
    const auto &available = this->badges_.availableChannel;
    const auto &selected = this->badges_.selectedChannelBadge;
    const auto needle = this->searchQuery_.trimmed();
    const auto scale = this->scale();

    auto *toggleButton = new BadgePickerToggleSwitch(this->contentWidget_);
    toggleButton->setChecked(this->badges_.useCustomChannelBadge);
    toggleButton->setEnabled(!this->actionInFlight_);
    toggleButton->updateMetrics(scale);

    QObject::connect(
        toggleButton, &QPushButton::clicked, this, [this](bool checked) {
            if (checked)
            {
                if (!this->badges_.selectedChannelBadge.setID.isEmpty())
                {
                    this->selectChannel(this->badges_.selectedChannelBadge);
                }
                else
                {
                    this->badges_.useCustomChannelBadge = true;
                    this->rebuildContent();
                }
            }
            else
            {
                this->deselectChannel();
            }
        });

    this->contentLayout_->addWidget(
        makeSettingRow("Use Custom Badge for This Channel", toggleButton,
                       this->contentWidget_, scale));

    if (this->badges_.subscriptionTier >= 2000)
    {
        auto *flairButton = new BadgePickerToggleSwitch(this->contentWidget_);
        flairButton->setChecked(!this->badges_.isBadgeModifierHidden);
        flairButton->setEnabled(!this->actionInFlight_);
        flairButton->updateMetrics(scale);

        QObject::connect(flairButton, &QPushButton::clicked, this,
                         [this](bool checked) {
                             this->setFlairHidden(!checked);
                         });

        this->contentLayout_->addWidget(
            makeSettingRow("Badge Flair for Tier 2 and 3 Subscriptions",
                           flairButton, this->contentWidget_, scale));
    }

    if (hasLeadModeratorChoice(this->badges_))
    {
        auto *leadModButton = new BadgePickerToggleSwitch(this->contentWidget_);
        leadModButton->setChecked(usesLeadModeratorBadge(this->badges_));
        leadModButton->setEnabled(!this->actionInFlight_);
        leadModButton->updateMetrics(scale);

        QObject::connect(leadModButton, &QPushButton::clicked, this,
                         [this](bool checked) {
                             const auto *badge = findAuthorityBadge(
                                 this->badges_.authorityBadges,
                                 checked ? QStringLiteral("lead_moderator")
                                         : QStringLiteral("moderator"));
                             if (badge == nullptr)
                             {
                                 return;
                             }
                             this->selectRole(*badge);
                         });

        this->contentLayout_->addWidget(
            makeSettingRow("Lead Moderator Badge", leadModButton,
                           this->contentWidget_, scale));
    }

    if (available.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
            this->setStatus("No channel-specific badges available.");
        return;
    }

    auto *label = new QLabel(
        QStringLiteral("Badges for #%1").arg(this->channel_->getName()),
        this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    const int gridColumns = this->badgeGridColumns();

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;
    int shown = 0;
    for (const auto &badge : available)
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_ &&
                         this->badges_.useCustomChannelBadge);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version &&
            this->badges_.useCustomChannelBadge)
            tile->setSelected(true);

        QObject::connect(tile, &QPushButton::clicked, this, [this, badge] {
            this->selectChannel(badge);
        });
        grid->addWidget(tile, row, col);
        ++shown;
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    }

    if (shown == 0)
    {
        if (needle.isEmpty())
        {
            this->setStatus("No channel-specific badges available.");
        }
        else
        {
            if (!this->statusIsError_)
            {
                this->setStatus({});
            }
            this->contentLayout_->addWidget(makeEmptyListLabel(
                QStringLiteral("No matching badges."), this->contentWidget_));
        }
        return;
    }

    if (!this->statusIsError_)
    {
        this->setStatus({});
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::loadEventBadges(bool force)
{
    if (this->eventBadgesLoading_)
        return;

    // cache de 10 minutos
    if (!force && this->eventBadgesLoaded_ &&
        this->eventBadgesCacheTime_.secsTo(QDateTime::currentDateTimeUtc()) <
            600)
        return;

    this->eventBadgesLoading_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;

    NetworkRequest("https://api.catquery.com/eventBadges")
        .onSuccess([self](const NetworkResult &result) {
            if (!self)
                return;
            self->eventBadgesLoading_ = false;
            self->eventBadgesLoaded_ = true;
            self->eventBadgesCacheTime_ = QDateTime::currentDateTimeUtc();
            self->eventBadgesCache_.clear();

            const auto arr =
                result.parseJsonValue().toObject().value("badges").toArray();
            for (const auto &val : arr)
            {
                const auto obj = val.toObject();
                EventBadge b;
                b.id = obj.value("id").toString();
                b.name = obj.value("name").toString();
                b.imageUrl = obj.value("imageUrl").toString();
                b.streamDatabaseUrl = obj.value("streamdatabaseUrl").toString();
                b.startAt = QDateTime::fromString(
                    obj.value("startAt").toString(), Qt::ISODate);
                b.endAt = QDateTime::fromString(obj.value("endAt").toString(),
                                                Qt::ISODate);
                if (!obj.value("free").isNull())
                    b.free = obj.value("free").toBool();
                self->eventBadgesCache_.push_back(b);
            }
            self->setStatus({});
            self->rebuildContent();
        })
        .onError([self](const NetworkResult &) {
            if (!self)
                return;
            self->eventBadgesLoading_ = false;
            self->setStatus("Failed to load event badges.", true);
            self->rebuildContent();
        })
        .execute();
}

void TwitchBadgePickerDialog::rebuildEventBadges()
{
    if (this->eventBadgesLoading_)
    {
        this->setStatus("Loading event badges...");
        return;
    }

    if (!this->eventBadgesLoaded_)
    {
        this->setStatus("No data loaded.");
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto needle = this->searchQuery_.trimmed();

    // set de IDs que o usuário já tem
    QSet<QString> ownedIds;
    for (const auto &b : this->badges_.availableGlobal)
        ownedIds.insert(b.setID);

    QVector<EventBadge> missing, comingSoon;
    for (const auto &b : this->eventBadgesCache_)
    {
        if (!needle.isEmpty() &&
            !b.name.contains(needle, Qt::CaseInsensitive) &&
            !b.id.contains(needle, Qt::CaseInsensitive))
            continue;

        if (b.startAt > now)
            comingSoon.push_back(b);
        else if (b.endAt > now && !ownedIds.contains(b.id))
            missing.push_back(b);
    }

    auto makeTile = [&](const EventBadge &badge, bool showPriceLabel,
                        QWidget *parent) -> QWidget * {
        GqlBadge gql;
        gql.setID = badge.id;
        gql.title = badge.name;
        gql.image2x = badge.imageUrl;
        BadgeTileButton *tile = nullptr;

        // tooltip com info da badge
        QString tip = badge.name;
        if (badge.endAt.isValid())
            tip += QStringLiteral("\nEnds: %1")
                       .arg(badge.endAt.toLocalTime().toString(
                           "MMM d, yyyy hh:mm"));
        if (badge.startAt.isValid() && badge.startAt > now)
            tip += QStringLiteral("\nStarts: %1")
                       .arg(badge.startAt.toLocalTime().toString(
                           "MMM d, yyyy hh:mm"));
        if (badge.free.has_value())
            tip += badge.free.value() ? "\nFree" : "\nPaid";
        if (!showPriceLabel)
        {
            tile = new BadgeTileButton(gql, parent);
            tile->setToolTip(tip);
            if (!badge.streamDatabaseUrl.isEmpty())
            {
                const auto url = badge.streamDatabaseUrl;
                QObject::connect(tile, &QPushButton::clicked, this, [url] {
                    QDesktopServices::openUrl(QUrl(url));
                });
            }
            return tile;
        }

        auto *cell = new QWidget(parent);
        auto *layout = new QVBoxLayout(cell);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        tile = new BadgeTileButton(gql, cell);
        tile->setToolTip(tip);
        if (!badge.streamDatabaseUrl.isEmpty())
        {
            const auto url = badge.streamDatabaseUrl;
            QObject::connect(tile, &QPushButton::clicked, this, [url] {
                QDesktopServices::openUrl(QUrl(url));
            });
        }
        layout->addWidget(tile, 0, Qt::AlignHCenter);

        auto *priceLabel = new QLabel(cell);
        priceLabel->setObjectName("TwitchBadgePickerEventBadgePrice");
        priceLabel->setAlignment(Qt::AlignHCenter);
        priceLabel->setText(badge.free.has_value()
                                ? (badge.free.value() ? "Free" : "Paid")
                                : QString());
        priceLabel->setFixedHeight(
            scaledMetric(this->scale(), /*base*/ 14, /*minimum*/ 10));
        layout->addWidget(priceLabel);

        return cell;
    };

    const int gridColumns = this->badgeGridColumns();

    auto makeGrid = [&](const QVector<EventBadge> &badges,
                        bool showPriceLabel) {
        auto *gridWidget = new QWidget(this->contentWidget_);
        auto *grid = new QGridLayout(gridWidget);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(BADGE_GRID_SPACING);
        int row = 0;
        int col = 0;
        for (const auto &badge : badges)
        {
            auto *tile = makeTile(badge, showPriceLabel, gridWidget);
            grid->addWidget(tile, row, col);
            if (++col >= gridColumns)
            {
                col = 0;
                ++row;
            }
        }
        this->contentLayout_->addWidget(gridWidget);
    };

    if (!missing.isEmpty())
    {
        auto *label =
            new QLabel("Missing (Available Now)", this->contentWidget_);
        label->setObjectName("TwitchBadgePickerSectionLabel");
        this->contentLayout_->addWidget(label);
        makeGrid(missing, true);
    }

    if (!comingSoon.isEmpty())
    {
        auto *label = new QLabel("Coming Soon", this->contentWidget_);
        label->setObjectName("TwitchBadgePickerSectionLabel");
        this->contentLayout_->addWidget(label);
        makeGrid(comingSoon, true);
    }

    if (missing.isEmpty() && comingSoon.isEmpty())
    {
        if (needle.isEmpty())
            this->setStatus("You already have all available event badges!");
        else
            this->setStatus("No event badges match your search.");
    }
    else if (!this->statusIsError_)
    {
        this->setStatus({});
    }
}

void TwitchBadgePickerDialog::switchView(View view)
{
    this->view_ = view;
    this->globalTabButton_->setChecked(view == View::GlobalBadges);
    this->channelTabButton_->setChecked(view == View::ChannelBadges);
    this->eventTabButton_->setChecked(view == View::EventBadges);
    this->colorTabButton_->setChecked(view == View::Color);
    this->bttvTabButton_->setChecked(view == View::Bttv);
    this->bttvEffectTabButton_->setChecked(view == View::BttvEffects);
    this->moltorinoTabButton_->setChecked(view == View::Moltorino);
    this->jilChatTabButton_->setChecked(view == View::JilChat);
    this->bluzyrinoTabButton_->setChecked(view == View::Bluzyrino);
    this->sevenTvBadgeTabButton_->setChecked(view == View::SevenTvBadges);
    this->sevenTvPaintTabButton_->setChecked(view == View::SevenTvPaints);
    this->updateSearchVisibility();

    if (view == View::EventBadges)
    {
        this->loadEventBadges(false);
    }
    else if (view == View::Moltorino)
    {
        this->loadMoltorinoBadges(false);
    }
    else if (view == View::JilChat)
    {
        this->loadJilChatBadges(false);
    }
    else if (view == View::Bttv || view == View::BttvEffects)
    {
        this->loadBttv(false);
    }
    else if (view == View::SevenTvBadges || view == View::SevenTvPaints)
    {
        this->loadSevenTv(false);
    }

    this->rebuildContent();
}

void TwitchBadgePickerDialog::updateSearchVisibility()
{
    if (this->searchRowWidget_ != nullptr)
    {
        this->searchRowWidget_->setVisible(this->view_ != View::Color &&
                                           this->view_ != View::Bttv);
    }
}

void TwitchBadgePickerDialog::rebuildColors()
{
    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        this->setStatus("You must be logged in to change your color.", true);
        return;
    }

    const auto currentColor = user->color();
    const auto currentHex = currentColor.isValid()
                                ? currentColor.name(QColor::HexRgb).toUpper()
                                : QString{};
    const auto selectedPreset = helixColorNameFromDisplayHex(currentHex);

    auto *label = new QLabel("Twitch Colors", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    const int gridColumns = this->badgeGridColumns();

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;
    for (const auto &name : VALID_HELIX_COLORS)
    {
        const auto hex = helixColorDisplayHex(name);
        if (hex.isEmpty())
        {
            continue;
        }

        auto *tile = new ColorTileButton(
            QColor(hex), formatHelixColorLabel(name), gridWidget);
        tile->setEnabled(!this->actionInFlight_);
        if (selectedPreset && *selectedPreset == name)
        {
            tile->setSelected(true);
        }

        QObject::connect(tile, &QPushButton::clicked, this, [this, name] {
            this->selectColor(name);
        });

        grid->addWidget(tile, row, col);
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    }
    this->contentLayout_->addWidget(gridWidget);

    auto *customLabel =
        new QLabel("Custom (Turbo / Prime)", this->contentWidget_);
    customLabel->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(customLabel);

    auto *customRow = new QWidget(this->contentWidget_);
    customRow->setObjectName("TwitchBadgePickerCustomColorRow");
    auto *customLayout = new QHBoxLayout(customRow);
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->setSpacing(scaledMetric(this->scale(), 6, 4));

    const int controlHeight = scaledMetric(this->scale(), 28, 22);
    const auto scale = this->scale();

    auto *hexInput = new QLineEdit(customRow);
    hexInput->setObjectName("TwitchBadgePickerHexInput");
    hexInput->setPlaceholderText("#RRGGBB");
    hexInput->setMaxLength(7);
    hexInput->setFixedHeight(controlHeight);
    hexInput->setFont(
        getApp()->getFonts()->getFont(FontStyle::ChatMediumMono, scale));
    hexInput->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^#?[0-9A-Fa-f]{0,6}$")), hexInput));
    if (!selectedPreset && !currentHex.isEmpty())
    {
        hexInput->setText(currentHex);
    }

    QColor swatchColor(QStringLiteral("#808080"));
    if (!selectedPreset && !currentHex.isEmpty())
    {
        swatchColor = QColor(currentHex);
    }

    auto *colorButton = new ColorButton(swatchColor, customRow);
    colorButton->setFixedSize(controlHeight, controlHeight);
    colorButton->setCursor(Qt::PointingHandCursor);
    colorButton->setToolTip("Open color picker");
    colorButton->setEnabled(!this->actionInFlight_);

    auto updatePreview = [colorButton, hexInput] {
        const auto text = normalizeCustomHexInput(hexInput->text());
        if (isValidCustomHexColor(text))
        {
            colorButton->setColor(QColor(text));
        }
        else
        {
            colorButton->setColor(QColor(0, 0, 0, 0));
        }
    };
    updatePreview();

    auto *applyButton = new QPushButton("Apply", customRow);
    applyButton->setObjectName("TwitchBadgePickerApplyButton");
    applyButton->setCursor(Qt::PointingHandCursor);
    applyButton->setFixedHeight(controlHeight);
    applyButton->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));

    auto updateApplyState = [this, applyButton, hexInput] {
        applyButton->setEnabled(
            !this->actionInFlight_ &&
            isValidCustomHexColor(normalizeCustomHexInput(hexInput->text())));
    };
    updateApplyState();

    QPointer<QLineEdit> hexInputPtr(hexInput);
    auto openColorPicker = [this, hexInputPtr, currentHex, updatePreview] {
        QColor initial(QStringLiteral("#808080"));
        if (hexInputPtr)
        {
            const auto text = normalizeCustomHexInput(hexInputPtr->text());
            if (isValidCustomHexColor(text))
            {
                initial = QColor(text);
            }
            else if (!currentHex.isEmpty())
            {
                initial = QColor(currentHex);
            }
        }
        else if (!currentHex.isEmpty())
        {
            initial = QColor(currentHex);
        }

        const bool wasAutoPinned = this->ensurePinned();

        auto *dialog = new ColorPickerDialog(initial, this);
        QObject::connect(
            dialog, &ColorPickerDialog::colorConfirmed, this,
            [hexInputPtr, updatePreview](const QColor &selected) {
                if (!selected.isValid() || !hexInputPtr)
                {
                    return;
                }
                hexInputPtr->setText(selected.name(QColor::HexRgb).toUpper());
                updatePreview();
            });
        QObject::connect(dialog, &QObject::destroyed, this,
                         [this, wasAutoPinned] {
                             if (wasAutoPinned)
                             {
                                 this->togglePinned();
                             }
                         });
        dialog->show();
    };

    QObject::connect(colorButton, &QAbstractButton::clicked, this,
                     openColorPicker);
    QObject::connect(
        hexInput, &QLineEdit::textChanged, customRow,
        [hexInput, updatePreview, updateApplyState] {
            const auto raw = hexInput->text();
            const auto normalized = normalizeCustomHexInput(raw);
            if (normalized != raw)
            {
                const auto cursor = hexInput->cursorPosition();
                const auto addedPrefix =
                    !raw.startsWith('#') && normalized.startsWith('#');
                hexInput->blockSignals(true);
                hexInput->setText(normalized);
                hexInput->setCursorPosition(cursor + (addedPrefix ? 1 : 0));
                hexInput->blockSignals(false);
            }
            updatePreview();
            updateApplyState();
        });
    QObject::connect(
        applyButton, &QPushButton::clicked, this, [this, hexInput] {
            const auto text = normalizeCustomHexInput(hexInput->text());
            if (!isValidCustomHexColor(text))
            {
                this->setStatus("Enter a valid hex color (#RRGGBB).", true);
                this->rebuildContent();
                return;
            }
            this->selectColor(text);
        });

    customLayout->addWidget(colorButton, 0, Qt::AlignVCenter);
    customLayout->addWidget(hexInput, 1);
    customLayout->addWidget(applyButton, 0, Qt::AlignVCenter);
    this->contentLayout_->addWidget(customRow);

    if (!selectedPreset && !currentHex.isEmpty() && !this->statusIsError_)
    {
        this->setStatus({});
    }
}

void TwitchBadgePickerDialog::selectColor(const QString &color)
{
    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        this->setStatus("You must be logged in to change your color.", true);
        this->rebuildContent();
        return;
    }

    auto colorString = color.trimmed();
    if (colorString.isEmpty())
    {
        return;
    }

    cleanHelixColorName(colorString);

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    getHelix()->updateUserChatColor(
        user->getUserId(), colorString,
        [self, colorString, user, channel{this->channel_}] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus({});

            if (colorString.startsWith('#'))
            {
                user->setColor(QColor(colorString));
            }
            else
            {
                const auto hex = helixColorDisplayHex(colorString);
                if (!hex.isEmpty())
                {
                    user->setColor(QColor(hex));
                }
            }

            channel->addSystemMessage(
                QStringLiteral("Your color has been changed to %1.")
                    .arg(colorString));
            self->rebuildContent();
        },
        [self, colorString](auto error, auto message) {
            if (!self)
                return;
            self->actionInFlight_ = false;

            QString errorMessage =
                QStringLiteral("Failed to change color to %1 - ")
                    .arg(colorString);

            switch (error)
            {
                case HelixUpdateUserChatColorError::UserMissingScope: {
                    errorMessage +=
                        "Missing required scope. Re-login with your "
                        "account and try again.";
                }
                break;

                case HelixUpdateUserChatColorError::InvalidColor: {
                    errorMessage +=
                        QStringLiteral("Color must be one of Twitch's "
                                       "supported colors (%1) or a "
                                       "hex code (#000000) if you "
                                       "have Turbo or Prime.")
                            .arg(VALID_HELIX_COLORS.join(", "));
                }
                break;

                case HelixUpdateUserChatColorError::Forwarded: {
                    errorMessage += message + ".";
                }
                break;

                case HelixUpdateUserChatColorError::Unknown:
                default: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            self->setStatus(errorMessage, true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::clearContent()
{
    // Its tile is about to go away.
    if (auto &tooltip = tileTooltip(); !tooltip.isNull())
    {
        tooltip->hide();
    }
    this->statusLabel_ = nullptr;
    while (auto *item = this->contentLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void TwitchBadgePickerDialog::setStatus(const QString &text, bool error)
{
    this->statusText_ = text;
    this->statusIsError_ = error;
    if (!this->statusLabel_)
        return;
    this->statusLabel_->setText(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    auto muted = this->theme->window.text;
    muted.setAlpha(150);
    const auto color =
        error ? QStringLiteral("#ff9e9e") : muted.name(QColor::HexArgb);
    this->statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
}

void TwitchBadgePickerDialog::applyPreviewDialogWidth(int contentPixelWidth)
{
    const auto scale = this->scale();
    const int chrome = contentHorizontalChrome(scale);
    int targetPixelWidth = std::max(int(DEFAULT_DIALOG_SIZE.width() * scale),
                                    contentPixelWidth + chrome);

    if (const auto *screen = QGuiApplication::primaryScreen())
    {
        const int maxWidth = screen->availableGeometry().width() * 9 / 10;
        targetPixelWidth = std::min(targetPixelWidth, maxWidth);
    }

    const int scaleIndependentWidth =
        std::max(this->scaleIndependentWidth(),
                 std::max(DEFAULT_DIALOG_SIZE.width(),
                          int(std::ceil(float(targetPixelWidth) / scale))));

    if (this->scaleIndependentWidth() != scaleIndependentWidth)
    {
        this->setScaleIndependentWidth(scaleIndependentWidth);
    }

    this->applySizeConstraints();
}

void TwitchBadgePickerDialog::updatePreview()
{
    if (this->previewView_ == nullptr || this->previewWidget_ == nullptr)
    {
        return;
    }

    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    const bool showPreview = user && !user->isAnon();
    this->previewWidget_->setVisible(showPreview);

    if (!showPreview)
    {
        this->previewView_->clearMessage();
        return;
    }

    const auto message = this->buildPreviewMessage();
    const int contentWidth = measureMessageContentWidth(
        message, this->scale(), float(this->devicePixelRatioF()));
    this->applyPreviewDialogWidth(contentWidth);
    this->previewView_->setWidth(contentWidth);
    this->previewView_->setFullMessage(message);
}

MessagePtr TwitchBadgePickerDialog::buildPreviewMessage() const
{
    if (this->channel_ == nullptr)
    {
        return nullptr;
    }

    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        return nullptr;
    }

    const QString userId = user->getUserId();
    const QString loginName = user->getUserName();
    const QString displayName =
        resolveSelfDisplayName(userId, loginName, this->channel_);
    const QColor userColor = user->color();
    const std::optional<QColor> color =
        userColor.isValid() ? std::optional(userColor) : std::nullopt;

    return MessageBuilder::makeSelfBadgePreviewMessage(
        this->channel_, userId, loginName, displayName, color,
        collectPreviewBadges(this->badges_, this->channel_, userId),
        toBadgePreviewFallbacks(gqlBadgeLookup(this->badges_)));
}

void TwitchBadgePickerDialog::selectGlobal(const GqlBadge &badge)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    const bool clearing = badge.setID.isEmpty();

    this->badges_.selectedGlobalBadge = clearing ? GqlBadge{} : badge;
    this->actionInFlight_ = true;
    this->rebuildContent();
    this->updatePreview();

    QPointer<TwitchBadgePickerDialog> self = this;
    TwitchGql::selectGlobalBadge(
        badge.setID, badge.version, token,
        [self, badge, clearing] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedGlobalBadge = clearing ? GqlBadge{} : badge;
            self->channel_->addSystemMessage(
                clearing ? QStringLiteral("Global badge cleared.")
                         : QStringLiteral("Global badge set to: %1")
                               .arg(badge.title));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("selecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::selectChannel(const GqlBadge &badge)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->badges_.selectedChannelBadge = badge;
    this->badges_.useCustomChannelBadge = true;
    this->actionInFlight_ = true;
    this->rebuildContent();
    this->updatePreview();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelId = this->channel_->roomId();

    TwitchGql::selectChannelBadge(
        badge.setID, badge.version, channelId, token,
        [self, badge] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedChannelBadge = badge;
            self->badges_.useCustomChannelBadge = true;
            self->channel_->addSystemMessage(
                QStringLiteral("Channel badge set to: %1").arg(badge.title));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("selecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::selectRole(const GqlBadge &badge)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->badges_.selectedAuthorityBadge = badge;
    this->actionInFlight_ = true;
    this->rebuildContent();
    this->updatePreview();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelId = this->channel_->roomId();

    TwitchGql::selectRoleBadge(
        badge.setID, badge.version, channelId, token,
        [self, badge] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedAuthorityBadge = badge;
            self->channel_->addSystemMessage(
                QStringLiteral("Role badge set to: %1").arg(badge.title));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "selecting role badge", error),
                            true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::deselectChannel()
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->badges_.useCustomChannelBadge = false;
    this->actionInFlight_ = true;
    this->rebuildContent();
    this->updatePreview();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelId = this->channel_->roomId();

    TwitchGql::deselectChannelBadge(
        channelId, token,
        [self] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.useCustomChannelBadge = false;
            self->channel_->addSystemMessage(
                QStringLiteral("Channel badge disabled."));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("deselecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::setFlairHidden(bool hidden)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->badges_.isBadgeModifierHidden = hidden;
    this->actionInFlight_ = true;
    this->rebuildContent();
    this->updatePreview();

    QPointer<TwitchBadgePickerDialog> self = this;

    TwitchGql::setBadgeModifierHidden(
        hidden, token,
        [self](bool actual) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.isBadgeModifierHidden = actual;
            self->channel_->addSystemMessage(
                actual ? QStringLiteral("Badge flair hidden.")
                       : QStringLiteral("Badge flair shown."));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "updating badge flair", error),
                            true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto rawScale = this->scale();
    const auto effectiveScale = rawScale;
    const int radius = std::max(1, int(2 * rawScale));
    const int inputPaddingX = std::max(4, int(5 * effectiveScale));
    const int inputMinHeight = std::max(14, int(20 * effectiveScale));
    const int scrollbarWidth = std::max(3, int(4 * effectiveScale));
    const int scrollbarRadius = std::max(1, int(2 * effectiveScale));
    const int scrollbarMinHeight = std::max(12, int(16 * effectiveScale));

    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, rawScale * 1.2F));
    this->globalTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->channelTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->eventTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->colorTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    if (this->searchInput_ != nullptr)
    {
        this->searchInput_->setFont(
            fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    const int customColorHeight = scaledMetric(effectiveScale, 28, 22);
    for (auto *hexInput : this->contentWidget_->findChildren<QLineEdit *>(
             "TwitchBadgePickerHexInput"))
    {
        hexInput->setFixedHeight(customColorHeight);
        hexInput->setFont(
            fonts->getFont(FontStyle::ChatMediumMono, effectiveScale));
    }
    for (auto *applyButton : this->contentWidget_->findChildren<QPushButton *>(
             "TwitchBadgePickerApplyButton"))
    {
        applyButton->setFixedHeight(customColorHeight);
        applyButton->setFont(
            fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    for (auto *colorButton :
         this->contentWidget_->findChildren<ColorButton *>(QString()))
    {
        if (colorButton->parent() &&
            colorButton->parent()->objectName() ==
                QStringLiteral("TwitchBadgePickerCustomColorRow"))
        {
            colorButton->setFixedSize(customColorHeight, customColorHeight);
        }
    }

    const int rowPaddingX = scaledMetric(effectiveScale, 8, 4);
    const int rowPaddingY = scaledMetric(effectiveScale, 6, 3);
    const int rowSpacing = scaledMetric(effectiveScale, 8, 4);
    for (auto *row : this->contentWidget_->findChildren<QWidget *>(
             "TwitchBadgePickerSettingRow"))
    {
        if (auto *rowLayout = qobject_cast<QHBoxLayout *>(row->layout()))
        {
            rowLayout->setContentsMargins(rowPaddingX, rowPaddingY, rowPaddingX,
                                          rowPaddingY);
            rowLayout->setSpacing(rowSpacing);
        }
    }
    for (auto *label : this->contentWidget_->findChildren<QLabel *>(
             "TwitchBadgePickerSettingLabel"))
    {
        label->setFont(fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    for (auto *button : this->contentWidget_->findChildren<QPushButton *>(
             "TwitchBadgePickerToggleSwitch"))
    {
        static_cast<BadgePickerToggleSwitch *>(button)->updateMetrics(rawScale);
    }

    const int hMargin = contentHorizontalMargin(rawScale);
    const int vMargin = std::max(3, int(5 * rawScale));
    this->headerWidget_->layout()->setContentsMargins(0, 0, 0, 0);
    this->mainLayout_->setContentsMargins(hMargin, vMargin, hMargin, vMargin);
    this->contentLayout_->setContentsMargins(
        0, scaledMetric(effectiveScale, 7, 4), 0,
        scaledMetric(effectiveScale, 8, 4));
    this->contentLayout_->setSpacing(scaledMetric(effectiveScale, 7, 4));
    if (auto *sep = this->findChild<QWidget *>(
            QStringLiteral("TwitchBadgePickerDialogSeparator")))
    {
        sep->setFixedHeight(scaledSeparatorHeight(rawScale));
    }

    const auto *theme = this->theme;
    auto textColor = theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(160);
    const auto bg = theme->window.background.name();
    const auto text = textColor.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto muted = mutedColor.name(QColor::HexArgb);
    const auto inputBg = theme->splits.input.background.name();
    const auto focusedBorder = theme->splits.header.focusedBorder.name();
    const auto hoverBg =
        theme->isLightTheme()
            ? theme->splits.input.background.darker(104).name()
            : theme->splits.input.background.lighter(108).name();

    this->closeButton_->setColor(textColor);

    this->setStyleSheet(QStringLiteral(R"(
        QWidget#TwitchBadgePickerDialogRoot {
            background: %1;
            color: %2;
        }
        QWidget#TwitchBadgePickerHeader {
            background: transparent;
        }
        QWidget#TwitchBadgePickerPreview {
            background: %5;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 4px 6px;
        }
        QFrame#TwitchBadgePickerDialogSeparator,
        QWidget#TwitchBadgePickerDialogSeparator {
            background: %3;
        }
        QScrollArea#TwitchBadgePickerScrollArea {
            background: transparent;
            border: 0;
        }
        QWidget#TwitchBadgePickerDialogContent {
            background: transparent;
            color: %2;
        }
        QLabel#TwitchBadgePickerTitle {
            color: %2;
            font-weight: 700;
        }
        QLabel#TwitchBadgePickerSectionLabel {
            color: %2;
            font-weight: 600;
            padding-top: 4px;
        }
        QLabel#TwitchBadgePickerEventBadgePrice {
            color: %4;
            font-size: 10px;
            font-weight: 600;
        }
        QLabel#TwitchBadgePickerStatus,
        QLabel#TwitchBadgePickerEmpty {
            color: %4;
        }
        QScrollBar:vertical {
            width: %10px;
            background: transparent;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: %3;
            min-height: %12px;
            border-radius: %11px;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: transparent;
            height: 0;
        }
        QPushButton#TwitchBadgePickerTab {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 3px 10px;
        }
        QPushButton#TwitchBadgePickerTab:checked {
            background: #9146ff;
            color: #ffffff;
            border-color: #9146ff;
        }
        QPushButton#TwitchBadgePickerTab:hover:!checked {
            background: %8;
            border-color: %7;
        }
        QLineEdit#TwitchBadgePickerSearch {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 0 %9px;
            min-height: %13px;
        }
        QLineEdit#TwitchBadgePickerSearch:focus {
            border-color: %7;
        }
        QLineEdit#TwitchBadgePickerHexInput {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 0 %9px;
            min-height: %13px;
        }
        QLineEdit#TwitchBadgePickerHexInput:focus {
            border-color: %7;
        }
        QPushButton#TwitchBadgePickerApplyButton {
            background: #9146ff;
            color: #ffffff;
            border: 1px solid #9146ff;
            border-radius: %6px;
            padding: 0 14px;
            min-height: %13px;
            font-weight: 600;
        }
        QPushButton#TwitchBadgePickerApplyButton:hover:enabled {
            background: #a970ff;
            border-color: #a970ff;
        }
        QPushButton#TwitchBadgePickerApplyButton:disabled {
            background: %5;
            color: %4;
            border-color: %3;
            font-weight: 400;
        }
        QWidget#TwitchBadgePickerCustomColorRow {
            background: transparent;
        }
        QWidget#TwitchBadgePickerSettingRow {
            background: %5;
            border: 1px solid %3;
            border-radius: %6px;
        }
        QLabel#TwitchBadgePickerSettingLabel {
            color: %2;
        }
    )")
                            .arg(bg, text, border, muted, inputBg,
                                 QString::number(radius), focusedBorder,
                                 hoverBg, QString::number(inputPaddingX),
                                 QString::number(scrollbarWidth),
                                 QString::number(scrollbarRadius),
                                 QString::number(scrollbarMinHeight),
                                 QString::number(inputMinHeight)));
}

void TwitchBadgePickerDialog::applySizeConstraints()
{
    const int requiredW =
        std::max(1, int(this->scaleIndependentWidth() * this->scale()));
    const int requiredH =
        std::max(1, int(this->scaleIndependentHeight() * this->scale()));

    const int minW = std::max(this->minimumWidth(), requiredW);
    const int minH = std::max(this->minimumHeight(), requiredH);

    this->setMinimumSize(minW, minH);
    this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    if (this->width() < minW || this->height() < minH)
    {
        this->resize(std::max(this->width(), minW),
                     std::max(this->height(), minH));
    }
}

void TwitchBadgePickerDialog::loadMoltorinoBadges(bool force)
{
    if (this->moltorinoLoading_ || (this->moltorinoLoaded_ && !force))
    {
        return;
    }

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        this->rebuildContent();
        return;
    }

    this->moltorinoLoading_ = true;
    this->setStatus("Loading Moltorino badges...");
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto fail = [self](const NetworkResult &result) {
        if (!self)
        {
            return;
        }
        self->moltorinoLoading_ = false;
        self->setStatus(QStringLiteral("Could not load Moltorino badges: %1")
                            .arg(result.formatError()),
                        true);
        self->rebuildContent();
    };

    // All badges first, then which ones the user owns and picked.
    NetworkRequest(QString::fromLatin1(MOLTORINO_BADGES_URL))
        .timeout(15000)
        .onSuccess([self, token, fail](const NetworkResult &list) {
            if (!self)
            {
                return;
            }
            const auto badges = list.parseJson().value("badges").toArray();

            NetworkRequest(QString::fromLatin1(MOLTORINO_PROFILE_URL))
                .header("Authorization", "OAuth " + token.toUtf8())
                .header("Accept", "application/json")
                .timeout(15000)
                .onSuccess([self, badges](const NetworkResult &result) {
                    if (!self)
                    {
                        return;
                    }
                    const auto root = result.parseJson();
                    auto profile = root.value("profile").toObject();
                    if (profile.isEmpty())
                    {
                        profile = root;
                    }

                    // Owned badges, as ids or objects with an id.
                    auto assigned = profile.value("assignedBadges").toArray();
                    if (assigned.isEmpty())
                    {
                        assigned = root.value("assignedBadges").toArray();
                    }
                    QSet<QString> owned;
                    for (const auto &value : std::as_const(assigned))
                    {
                        owned.insert(
                            value.isObject()
                                ? value.toObject().value("id").toString()
                                : value.toString());
                    }

                    self->moltorinoBadges_.clear();
                    for (const auto &value : badges)
                    {
                        const auto badge = value.toObject();
                        const auto id = badge.value("id").toString();
                        const auto images = badge.value("images").toObject();
                        if (id.isEmpty())
                        {
                            continue;
                        }
                        const bool isOwned = owned.contains(id);
                        // Unlisted badges only show up for people who own them.
                        if (!isOwned && !badge.value("listed").toBool(true))
                        {
                            continue;
                        }
                        self->moltorinoBadges_.push_back({
                            .badge =
                                {
                                    .id = id,
                                    .setID = id,
                                    .title =
                                        badge.value("tooltip").toString(id),
                                    .description =
                                        badge.value("description").toString(),
                                    .image1x = images.value("1x").toString(),
                                    .image2x = images.value("2x").toString(),
                                    .image4x = images.value("3x").toString(),
                                },
                            .owned = isOwned,
                        });
                    }
                    // Owned badges first, like in Moltorino.
                    std::ranges::stable_partition(self->moltorinoBadges_,
                                                  [](const auto &option) {
                                                      return option.owned;
                                                  });

                    self->moltorinoProfile_ = profile;
                    self->moltorinoSelected_ =
                        profile.value("selectedBadge").toString();
                    self->moltorinoLoading_ = false;
                    self->moltorinoLoaded_ = true;
                    self->setStatus({});
                    self->rebuildContent();
                })
                .onError(fail)
                .execute();
        })
        .onError(fail)
        .execute();
}

void TwitchBadgePickerDialog::rebuildMoltorinoBadges()
{
    if (this->moltorinoLoading_ || !this->moltorinoLoaded_)
    {
        return;
    }

    const auto needle = this->searchQuery_.trimmed();
    auto *label = new QLabel("Moltorino badge", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    const int gridColumns = this->badgeGridColumns();
    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;
    const auto next = [&] {
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    };

    if (needle.isEmpty())
    {
        auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
        noBadgeTile->setEnabled(!this->actionInFlight_);
        noBadgeTile->setSelected(this->moltorinoSelected_.isEmpty());
        QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
            this->selectMoltorino({});
        });
        grid->addWidget(noBadgeTile, row, col);
        next();
    }

    int shown = 0;
    for (const auto &option : std::as_const(this->moltorinoBadges_))
    {
        if (!badgeMatchesSearch(option.badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(option.badge, gridWidget);
        tile->setSelected(option.badge.id == this->moltorinoSelected_);
        if (option.owned)
        {
            tile->setEnabled(!this->actionInFlight_);
            QObject::connect(tile, &QPushButton::clicked, this,
                             [this, id = option.badge.id] {
                                 this->selectMoltorino(id);
                             });
        }
        else
        {
            // Not unlocked: shown dimmed for a preview, like in Moltorino.
            tile->setEnabled(false);
            tile->setToolTip(option.badge.title + QStringLiteral(" (locked)"));
            auto *dim = new QGraphicsOpacityEffect(tile);
            dim->setOpacity(0.35);
            tile->setGraphicsEffect(dim);
        }
        grid->addWidget(tile, row, col);
        ++shown;
        next();
    }

    if (shown == 0 && !needle.isEmpty())
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("No matching badges."), this->contentWidget_));
        return;
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::loadJilChatBadges(bool force)
{
    if (this->jilChatLoading_ || (this->jilChatLoaded_ && !force))
    {
        return;
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        this->setStatus("Log in to pick your JilChat badge.", true);
        return;
    }

    this->jilChatLoading_ = true;
    if (!this->jilChatLoaded_)
    {
        this->setStatus("Loading JilChat badges...");
    }

    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto fail = [self](const NetworkResult &result) {
        if (!self)
        {
            return;
        }
        self->jilChatLoading_ = false;
        self->setStatus(QStringLiteral("Could not load JilChat badges: %1")
                            .arg(result.formatError()),
                        true);
        self->rebuildContent();
    };

    // All badges first, then which ones the user owns and shows. Both lists
    // are public; no login is sent here.
    NetworkRequest(QString::fromLatin1(JILCHAT_ALL_BADGES_URL))
        .timeout(15000)
        .onSuccess([self, fail,
                    userId = account->getUserId()](const NetworkResult &all) {
            if (!self)
            {
                return;
            }
            const auto allBadges = all.parseJsonArray();

            NetworkRequest(
                QString::fromLatin1(JILCHAT_USER_BADGES_URL).arg(userId))
                .timeout(15000)
                .onSuccess([self, allBadges](const NetworkResult &result) {
                    if (!self)
                    {
                        return;
                    }
                    const auto makeOption = [](const QJsonObject &badge,
                                               const QString &badgeId,
                                               bool owned) {
                        const auto slug = badge.value("slug").toString();
                        const auto image = badge.value("image_url").toString();
                        return JilChatBadgeOption{
                            .badge =
                                {
                                    .id = slug,
                                    .setID = slug,
                                    .title = badge.value("name").toString(slug),
                                    .image1x = image,
                                    .image2x = image,
                                    .image4x = image,
                                },
                            .badgeId = badgeId,
                            .owned = owned,
                        };
                    };

                    // The user's badges, by slug.
                    QHash<QString, QJsonObject> owned;
                    self->jilChatSelected_.clear();
                    const auto ownedBadges = result.parseJsonArray();
                    for (const auto &value : ownedBadges)
                    {
                        const auto badge = value.toObject();
                        const auto slug = badge.value("slug").toString();
                        if (slug.isEmpty())
                        {
                            continue;
                        }
                        owned.insert(slug, badge);
                        // `visible` is the older name of `active`.
                        if (badge.value("active").toBool(
                                badge.value("visible").toBool()))
                        {
                            self->jilChatSelected_ = slug;
                        }
                    }

                    self->jilChatBadges_.clear();
                    QSet<QString> listed;
                    for (const auto &value : allBadges)
                    {
                        const auto badge = value.toObject();
                        const auto slug = badge.value("slug").toString();
                        if (slug.isEmpty() ||
                            badge.value("image_url").toString().isEmpty())
                        {
                            continue;
                        }
                        listed.insert(slug);
                        const auto mine = owned.constFind(slug);
                        self->jilChatBadges_.push_back(
                            mine == owned.constEnd()
                                ? makeOption(badge,
                                             badge.value("id").toString(),
                                             false)
                                : makeOption(badge,
                                             mine->value("badge_id").toString(),
                                             true));
                    }
                    // Whatever the user owns that the list of all badges
                    // doesn't have.
                    for (const auto &value : ownedBadges)
                    {
                        const auto badge = value.toObject();
                        const auto slug = badge.value("slug").toString();
                        if (!slug.isEmpty() && !listed.contains(slug) &&
                            !badge.value("image_url").toString().isEmpty())
                        {
                            self->jilChatBadges_.push_back(makeOption(
                                badge, badge.value("badge_id").toString(),
                                true));
                        }
                    }

                    // The user's badges first; the milestones after the
                    // others, by their months.
                    std::ranges::stable_sort(
                        self->jilChatBadges_, [](const auto &a, const auto &b) {
                            if (a.owned != b.owned)
                            {
                                return a.owned;
                            }
                            return jilChatMilestoneMonths(a.badge.id) <
                                   jilChatMilestoneMonths(b.badge.id);
                        });

                    self->jilChatLoading_ = false;
                    self->jilChatLoaded_ = true;
                    if (!self->statusIsError_)
                    {
                        self->setStatus({});
                    }
                    self->rebuildContent();
                })
                .onError(fail)
                .execute();
        })
        .onError(fail)
        .execute();
}

void TwitchBadgePickerDialog::rebuildJilChatBadges()
{
    if (!this->jilChatLoaded_)
    {
        return;
    }

    const auto needle = this->searchQuery_.trimmed();
    auto *label = new QLabel("JilChat badge", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    const int gridColumns = this->badgeGridColumns();
    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;
    const auto next = [&] {
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    };

    if (needle.isEmpty())
    {
        auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
        noBadgeTile->setEnabled(!this->actionInFlight_);
        noBadgeTile->setSelected(this->jilChatSelected_.isEmpty());
        QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
            this->selectJilChat({});
        });
        grid->addWidget(noBadgeTile, row, col);
        next();
    }

    int shown = 0;
    for (const auto &option : std::as_const(this->jilChatBadges_))
    {
        if (!badgeMatchesSearch(option.badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(option.badge, gridWidget);
        tile->setSelected(option.badge.id == this->jilChatSelected_);
        if (option.owned)
        {
            tile->setEnabled(!this->actionInFlight_);
            QObject::connect(tile, &QPushButton::clicked, this,
                             [this, slug = option.badge.id] {
                                 this->selectJilChat(slug);
                             });
        }
        else
        {
            // Not unlocked: shown dimmed, to see what there is.
            tile->setEnabled(false);
            tile->setToolTip(option.badge.title + QStringLiteral(" (locked)"));
            auto *dim = new QGraphicsOpacityEffect(tile);
            dim->setOpacity(0.35);
            tile->setGraphicsEffect(dim);
        }
        grid->addWidget(tile, row, col);
        ++shown;
        next();
    }

    if (shown == 0 && !needle.isEmpty())
    {
        delete gridWidget;
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("No matching badges."), this->contentWidget_));
        return;
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::rebuildBluzyrinoBadges()
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("Log in to pick your Bluzyrino badges."),
            this->contentWidget_));
        return;
    }

    auto *bluzyrino = getApp()->getBluzyrinoBadges();
    const auto catalog = bluzyrino->catalog();
    if (catalog.empty())
    {
        // The registry is read shortly after the start and then again and
        // again; the tab is filled once it's there.
        bluzyrino->loadBluzyrinoBadges();
        this->contentLayout_->addWidget(
            makeEmptyListLabel(QStringLiteral("Loading Bluzyrino badges..."),
                               this->contentWidget_));
        return;
    }

    const auto userId = account->getUserId();
    const auto owned = bluzyrino->availableBadgeIds(userId);
    const auto isOwned = [&owned](const QString &id) {
        return std::ranges::find(owned, id) != owned.end();
    };
    const auto donor = QStringLiteral("donor");

    // The donor badge shown in chat: the picked one, else the first owned.
    auto selected = bluzyrino->selectedDonor(userId);
    if (!this->bluzyrinoPending_.isEmpty())
    {
        if (selected == this->bluzyrinoPending_)
        {
            this->bluzyrinoPending_.clear();
        }
        else
        {
            selected = this->bluzyrinoPending_;
        }
    }
    if (selected.isEmpty() || !isOwned(selected))
    {
        selected.clear();
        for (const auto &badge : catalog)
        {
            if (badge.category == donor && isOwned(badge.id))
            {
                selected = badge.id;
                break;
            }
        }
    }

    if (bluzyrino->ownsFounder(userId))
    {
        const auto scale = this->scale();
        auto *founderButton = new BadgePickerToggleSwitch(this->contentWidget_);
        founderButton->setChecked(
            getSettings()->bluzyrinoFounderVisible.getValue());
        founderButton->updateMetrics(scale);
        QObject::connect(
            founderButton, &QPushButton::clicked, this, [](bool checked) {
                getSettings()->bluzyrinoFounderVisible.setValue(checked);
                getApp()->getBluzyrinoBadges()->setFounderVisible(checked);
            });
        this->contentLayout_->addWidget(
            makeSettingRow("Show my Founder badge to other Bluzyrino users",
                           founderButton, this->contentWidget_, scale));
    }

    const auto needle = this->searchQuery_.trimmed();
    const int gridColumns = this->badgeGridColumns();
    int shown = 0;
    const auto addSection = [&](const QString &title, const QString &category,
                                bool pickable) {
        QWidget *gridWidget = nullptr;
        QGridLayout *grid = nullptr;
        int index = 0;
        for (const auto &badge : catalog)
        {
            if (badge.category != category || !badge.emote)
            {
                continue;
            }

            GqlBadge tileBadge;
            tileBadge.id = badge.id;
            tileBadge.setID = badge.id;
            tileBadge.title = badge.emote->tooltip.string;
            tileBadge.image1x = badge.emote->images.getImage1()->url().string;
            tileBadge.image2x = badge.emote->images.getImage2()->url().string;
            tileBadge.image4x = badge.emote->images.getImage3()->url().string;
            if (!badgeMatchesSearch(tileBadge, needle))
            {
                continue;
            }

            if (grid == nullptr)
            {
                auto *label = new QLabel(title, this->contentWidget_);
                label->setObjectName("TwitchBadgePickerSectionLabel");
                this->contentLayout_->addWidget(label);

                gridWidget = new QWidget(this->contentWidget_);
                grid = new QGridLayout(gridWidget);
                grid->setContentsMargins(0, 0, 0, 0);
                grid->setSpacing(BADGE_GRID_SPACING);
            }

            auto *tile = new BadgeTileButton(tileBadge, gridWidget);
            if (!isOwned(badge.id))
            {
                // Not unlocked: shown dimmed, to see what there is.
                tile->setEnabled(false);
                tile->setToolTip(tileBadge.title + QStringLiteral(" (locked)"));
                auto *dim = new QGraphicsOpacityEffect(tile);
                dim->setOpacity(0.35);
                tile->setGraphicsEffect(dim);
            }
            else if (pickable)
            {
                tile->setSelected(badge.id == selected);
                tile->setEnabled(!this->actionInFlight_);
                if (badge.id != selected)
                {
                    QObject::connect(
                        tile, &QPushButton::clicked, this,
                        [this, id = badge.id, name = tileBadge.title] {
                            this->selectBluzyrino(id, name);
                        });
                }
            }
            else
            {
                // Owned and always shown; there is nothing to pick.
                tile->setCursor(Qt::ArrowCursor);
                tile->setFocusPolicy(Qt::NoFocus);
            }
            grid->addWidget(tile, index / gridColumns, index % gridColumns);
            ++index;
            ++shown;
        }

        if (gridWidget != nullptr)
        {
            this->contentLayout_->addWidget(gridWidget);
        }
    };

    addSection(QStringLiteral("Donor badge"), donor, true);
    addSection(QStringLiteral("Special badges (always shown)"),
               QStringLiteral("special"), false);

    if (shown == 0)
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            needle.isEmpty() ? QStringLiteral("No Bluzyrino badges available.")
                             : QStringLiteral("No matching badges."),
            this->contentWidget_));
    }
}

void TwitchBadgePickerDialog::selectBluzyrino(const QString &badgeId,
                                              const QString &title)
{
    if (this->actionInFlight_)
    {
        return;
    }

    this->actionInFlight_ = true;
    this->bluzyrinoPending_ = badgeId;
    this->setStatus({});
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    getApp()->getBluzyrinoBadges()->setDonorSelection(badgeId, [self, title](
                                                                   bool ok) {
        if (!self)
        {
            return;
        }
        self->actionInFlight_ = false;
        if (ok)
        {
            self->channel_->addSystemMessage(
                QStringLiteral("Bluzyrino badge set to: %1").arg(title));
        }
        else
        {
            self->bluzyrinoPending_.clear();
            self->setStatus(
                QStringLiteral("Could not change the Bluzyrino badge."), true);
        }
        self->rebuildContent();
    });
}

void TwitchBadgePickerDialog::selectJilChat(const QString &slug)
{
    if (slug == this->jilChatSelected_ || this->actionInFlight_)
    {
        return;
    }
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        this->setStatus("Log in to pick your JilChat badge.", true);
        return;
    }

    QJsonObject body;
    QString name;
    if (slug.isEmpty())
    {
        body.insert("type", "none");
    }
    else
    {
        const auto option =
            std::ranges::find(this->jilChatBadges_, slug, [](const auto &o) {
                return o.badge.id;
            });
        if (option == this->jilChatBadges_.end())
        {
            return;
        }
        name = option->badge.title;
        if (const auto months = jilChatMilestoneMonths(slug); months >= 0)
        {
            body.insert("type", "milestone");
            body.insert("months", months);
        }
        else
        {
            body.insert("type", "custom");
            body.insert("badge_id", option->badgeId);
        }
    }

    const auto previous = this->jilChatSelected_;
    this->jilChatSelected_ = slug;
    this->actionInFlight_ = true;
    this->setStatus({});
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    // JilChat checks the Twitch login with Twitch and takes the user from
    // there, so the token and the client it was made for go along.
    NetworkRequest(QString::fromLatin1(JILCHAT_ACTIVE_BADGE_URL),
                   NetworkRequestType::Put)
        .header("Authorization", "Bearer " + account->getOAuthToken().toUtf8())
        .header("Client-Id", account->getOAuthClient().toUtf8())
        .header("Accept", "application/json")
        .json(body)
        .timeout(15000)
        .onSuccess([self, name](const NetworkResult &) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->channel_->addSystemMessage(
                name.isEmpty()
                    ? QStringLiteral("JilChat badge cleared.")
                    : QStringLiteral("JilChat badge set to: %1").arg(name));
            self->loadJilChatBadges(true);
            // The badges shown in chat.
            getApp()->getJilChatBadges()->loadJilChatBadges();
        })
        .onError([self, previous](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->jilChatSelected_ = previous;

            QString message;
            if (result.status() == 401)
            {
                message = QStringLiteral(
                    "JilChat didn't accept your Twitch login. Log in to "
                    "Twitch again and retry.");
            }
            else if (result.status() == 403)
            {
                message = QStringLiteral(
                    "Not allowed: changing the badge needs JilChat Pro, and "
                    "the badge has to be one of yours.");
            }
            else if (result.status() == 404)
            {
                message = QStringLiteral(
                    "There is no JilChat account for this Twitch account.");
            }
            else if (result.status() == 429)
            {
                message = QStringLiteral(
                    "Too many badge changes. Try again in a minute.");
            }
            else
            {
                message =
                    QStringLiteral("Could not change the JilChat badge: %1")
                        .arg(result.formatError());
            }
            self->setStatus(message, true);
            self->rebuildContent();
        })
        .execute();
}

void TwitchBadgePickerDialog::selectMoltorino(const QString &badgeId)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        return;
    }

    // Keep the order and hidden badges, only change the selection.
    const auto &profile = this->moltorinoProfile_;
    QJsonObject body{
        {"revision", profile.value("revision")},
        {"layoutSchemaVersion", profile.value("layoutSchemaVersion").toInt(1)},
        {"order", profile.value("order").toArray()},
        {"hidden", profile.value("hidden").toArray()},
        {"selectedBadge", badgeId.isEmpty() ? QJsonValue(QJsonValue::Null)
                                            : QJsonValue(badgeId)},
        {"badgeSelectionExplicit", true},
    };

    const auto previous = this->moltorinoSelected_;
    this->moltorinoSelected_ = badgeId;
    this->actionInFlight_ = true;
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    NetworkRequest(QString::fromLatin1(MOLTORINO_PROFILE_URL),
                   NetworkRequestType::Put)
        .header("Authorization", "OAuth " + token.toUtf8())
        .header("Accept", "application/json")
        .json(body)
        .timeout(15000)
        .onSuccess([self, badgeId](const NetworkResult &) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->channel_->addSystemMessage(
                badgeId.isEmpty() ? QStringLiteral("Moltorino badge cleared.")
                                  : QStringLiteral("Moltorino badge set to: %1")
                                        .arg(badgeId));
            // New revision for the next change; badges for the chat.
            self->loadMoltorinoBadges(true);
            getApp()->getMoltorinoSupporterBadges()->refreshNow();
        })
        .onError([self, previous](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->moltorinoSelected_ = previous;
            self->setStatus(
                QStringLiteral("Could not change the Moltorino badge: %1")
                    .arg(result.formatError()),
                true);
            // The profile may have changed elsewhere; load it again.
            self->moltorinoLoaded_ = false;
            self->loadMoltorinoBadges(true);
        })
        .execute();
}

void TwitchBadgePickerDialog::sevenTvRequest(
    const QString &query, const QJsonObject &variables,
    const std::function<void(const QJsonObject &)> &onData,
    const std::function<void(const QString &)> &onError)
{
    const QPointer<TwitchBadgePickerDialog> self = this;
    NetworkRequest(QString::fromLatin1(SEVENTV_GQL_URL),
                   NetworkRequestType::Post)
        .header("Authorization", "Bearer " + this->sevenTvToken_.toUtf8())
        .json(QJsonObject{{"query", query}, {"variables", variables}})
        .timeout(15000)
        .onSuccess([self, onData, onError](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            const auto root = result.parseJson();
            const auto errors = root.value("errors").toArray();
            if (!errors.isEmpty())
            {
                onError(errors.first().toObject().value("message").toString(
                    QStringLiteral("7TV rejected the request")));
                return;
            }
            onData(root.value("data").toObject());
        })
        .onError([self, onError](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            if (result.status() == 401)
            {
                onError(QStringLiteral(
                    "Your 7TV connection expired. Connect it again."));
                return;
            }
            onError(result.formatError());
        })
        .execute();
}

void TwitchBadgePickerDialog::loadSevenTv(bool force)
{
    if (this->sevenTvLoading_ || (this->sevenTvLoaded_ && !force))
    {
        return;
    }

    const auto twitchUserId =
        getApp()->getAccounts()->twitch.getCurrent()->getUserId();

    // The token is read from the credential store once.
    if (!this->sevenTvTokenRead_)
    {
        this->sevenTvLoading_ = true;
        this->setStatus("Loading 7TV...");
        this->rebuildContent();
        Credentials::instance().get(
            QString::fromLatin1(SEVENTV_CREDENTIAL_PROVIDER), twitchUserId,
            this, [this](const QString &token) {
                this->sevenTvTokenRead_ = true;
                this->sevenTvToken_ = token;
                this->sevenTvLoading_ = false;
                this->setStatus({});
                this->loadSevenTv(true);
            });
        return;
    }

    if (this->sevenTvToken_.isEmpty())
    {
        this->rebuildContent();
        return;
    }

    this->sevenTvLoading_ = true;
    this->setStatus("Loading 7TV badges...");
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    this->sevenTvRequest(
        QString::fromLatin1(SEVENTV_INVENTORY_QUERY), {},
        [self, twitchUserId](const QJsonObject &data) {
            self->sevenTvLoading_ = false;
            const auto me =
                data.value("users").toObject().value("me").toObject();

            // Only use a 7TV account that belongs to the current Twitch
            // account.
            bool sameAccount = false;
            const auto connections = me.value("connections").toArray();
            for (const auto &value : connections)
            {
                const auto connection = value.toObject();
                if (connection.value("platform")
                            .toString()
                            .compare("TWITCH", Qt::CaseInsensitive) == 0 &&
                    connection.value("platformId").toString() == twitchUserId)
                {
                    sameAccount = true;
                }
            }
            if (me.isEmpty() || !sameAccount)
            {
                self->setStatus(
                    me.isEmpty()
                        ? QStringLiteral("7TV did not accept that token.")
                        : QStringLiteral("This 7TV connection belongs to a "
                                         "different Twitch account."),
                    true);
                self->rebuildContent();
                return;
            }

            self->sevenTvUserId_ = me.value("id").toString();
            const auto style = me.value("style").toObject();
            self->sevenTvActiveBadge_ = style.value("activeBadgeId").toString();
            self->sevenTvActivePaint_ = style.value("activePaintId").toString();

            const auto inventory = me.value("inventory").toObject();
            self->sevenTvBadges_.clear();
            const auto inventoryBadges = inventory.value("badges").toArray();
            for (const auto &value : inventoryBadges)
            {
                const auto badge = value.toObject()
                                       .value("to")
                                       .toObject()
                                       .value("badge")
                                       .toObject();
                const auto id = badge.value("id").toString();
                if (id.isEmpty())
                {
                    continue;
                }
                // Images come in scales 1 to 4.
                std::array<QString, 4> images;
                const auto imageList = badge.value("images").toArray();
                for (const auto &imageValue : imageList)
                {
                    const auto image = imageValue.toObject();
                    const auto scale = image.value("scale").toInt();
                    const auto url = image.value("url").toString();
                    if (scale < 1 || scale > 4)
                    {
                        continue;
                    }
                    auto &slot = images.at(static_cast<size_t>(scale - 1));
                    if (slot.isEmpty() || url.endsWith(".webp"))
                    {
                        slot = url;
                    }
                }
                self->sevenTvBadges_.push_back({
                    .id = id,
                    .setID = id,
                    .title = badge.value("name").toString(),
                    .image1x = images[0],
                    .image2x = images[1],
                    .image4x = images[3].isEmpty() ? images[2] : images[3],
                });
            }

            self->sevenTvPaints_.clear();
            const auto inventoryPaints = inventory.value("paints").toArray();
            for (const auto &value : inventoryPaints)
            {
                const auto paint = value.toObject()
                                       .value("to")
                                       .toObject()
                                       .value("paint")
                                       .toObject();
                const auto id = paint.value("id").toString();
                if (!id.isEmpty())
                {
                    self->sevenTvPaints_.push_back(
                        {.id = id, .name = paint.value("name").toString()});
                }
            }

            self->sevenTvLoaded_ = true;
            self->setStatus({});
            self->rebuildContent();
            self->loadSevenTvPaintData();
        },
        [self](const QString &error) {
            self->sevenTvLoading_ = false;
            self->setStatus(error, true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::loadSevenTvPaintData()
{
    QJsonArray missing;
    for (const auto &paint : std::as_const(this->sevenTvPaints_))
    {
        if (!getApp()->getSeventvPaints()->getPaintById(paint.id))
        {
            missing.append(paint.id);
        }
    }
    if (missing.isEmpty())
    {
        return;
    }

    const QPointer<TwitchBadgePickerDialog> self = this;
    NetworkRequest(QString::fromLatin1(SEVENTV_V3_GQL_URL),
                   NetworkRequestType::Post)
        .json(QJsonObject{
            {"query", QString::fromLatin1(SEVENTV_PAINTS_QUERY)},
            {"variables", QJsonObject{{"list", missing}}},
        })
        .timeout(15000)
        .onSuccess([self](const NetworkResult &result) {
            const auto paints = result.parseJson()
                                    .value("data")
                                    .toObject()
                                    .value("cosmetics")
                                    .toObject()
                                    .value("paints")
                                    .toArray();
            for (const auto &paint : paints)
            {
                getApp()->getSeventvPaints()->addPaint(paint.toObject());
            }
            if (self && self->view_ == View::SevenTvPaints)
            {
                self->rebuildContent();
            }
        })
        .execute();
}

void TwitchBadgePickerDialog::rebuildSevenTv()
{
    const bool paints = this->view_ == View::SevenTvPaints;
    const bool connected = !this->sevenTvToken_.isEmpty();

    // Title, hint and the connect button, like in Moltorino.
    auto *headerRow = new QWidget(this->contentWidget_);
    auto *header = new QHBoxLayout(headerRow);
    header->setContentsMargins(0, 0, 0, 0);
    auto *texts = new QVBoxLayout();
    texts->setSpacing(0);
    auto *title =
        new QLabel(paints ? "7TV paint" : "7TV badge", this->contentWidget_);
    title->setObjectName("TwitchBadgePickerSectionLabel");
    texts->addWidget(title);
    QString hintText;
    if (connected)
    {
        hintText = paints ? "Choose one of your 7TV paints."
                          : "Choose one of your 7TV badges.";
    }
    else
    {
        hintText = paints ? "Connect 7TV to choose a paint."
                          : "Connect 7TV to choose a badge.";
    }
    auto *hint = new QLabel(hintText, this->contentWidget_);
    hint->setWordWrap(true);
    texts->addWidget(hint);
    header->addLayout(texts, 1);
    auto *connectButton =
        new QPushButton(connected ? "Disconnect" : "Connect", headerRow);
    connectButton->setCursor(Qt::PointingHandCursor);
    connectButton->setEnabled(!this->sevenTvLoading_ && !this->actionInFlight_);
    QObject::connect(connectButton, &QPushButton::clicked, this,
                     [this, connected] {
                         // Later: both rebuild the content, which deletes this
                         // button.
                         QTimer::singleShot(0, this, [this, connected] {
                             if (connected)
                             {
                                 this->disconnectSevenTv();
                             }
                             else
                             {
                                 this->connectSevenTv();
                             }
                         });
                     });
    header->addWidget(connectButton, 0, Qt::AlignTop);
    this->contentLayout_->addWidget(headerRow);

    if (!connected || this->sevenTvLoading_ || !this->sevenTvLoaded_)
    {
        return;
    }

    const auto needle = this->searchQuery_.trimmed();

    if (paints)
    {
        // Your name in each paint, two per row.
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        const auto userName = user->getUserName();
        auto userColor = user->color();
        if (!userColor.isValid())
        {
            userColor = this->theme->window.text;
        }

        auto *list = new QWidget(this->contentWidget_);
        auto *paintGrid = new QGridLayout(list);
        paintGrid->setContentsMargins(0, 0, 0, 0);
        paintGrid->setSpacing(BADGE_GRID_SPACING);
        constexpr int paintColumns = 2;
        int paintIndex = 0;
        const auto addPaint = [&](const QString &id, const QString &name) {
            auto *tile =
                new PaintTileButton(id, name, userName, userColor, list);
            tile->setSelected(id == this->sevenTvActivePaint_);
            tile->setEnabled(!this->actionInFlight_);
            QObject::connect(tile, &QPushButton::clicked, this, [this, id] {
                this->selectSevenTv(true, id);
            });
            paintGrid->addWidget(tile, paintIndex / paintColumns,
                                 paintIndex % paintColumns);
            ++paintIndex;
        };
        if (needle.isEmpty())
        {
            addPaint({}, "No paint");
        }
        int shown = 0;
        for (const auto &paint : std::as_const(this->sevenTvPaints_))
        {
            if (paint.name.contains(needle, Qt::CaseInsensitive))
            {
                addPaint(paint.id, paint.name);
                ++shown;
            }
        }
        if (shown == 0 && !needle.isEmpty())
        {
            this->contentLayout_->addWidget(makeEmptyListLabel(
                QStringLiteral("No matching paints."), this->contentWidget_));
            return;
        }
        this->contentLayout_->addWidget(list);
        return;
    }

    const int gridColumns = this->badgeGridColumns();
    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);
    int row = 0;
    int col = 0;
    const auto next = [&] {
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    };

    if (needle.isEmpty())
    {
        auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
        noBadgeTile->setToolTip("No 7TV badge");
        noBadgeTile->setEnabled(!this->actionInFlight_);
        noBadgeTile->setSelected(this->sevenTvActiveBadge_.isEmpty());
        QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
            this->selectSevenTv(false, {});
        });
        grid->addWidget(noBadgeTile, row, col);
        next();
    }

    int shown = 0;
    for (const auto &badge : std::as_const(this->sevenTvBadges_))
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }
        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);
        tile->setSelected(badge.id == this->sevenTvActiveBadge_);
        QObject::connect(tile, &QPushButton::clicked, this,
                         [this, id = badge.id] {
                             this->selectSevenTv(false, id);
                         });
        grid->addWidget(tile, row, col);
        ++shown;
        next();
    }

    if (shown == 0 && !needle.isEmpty())
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("No matching badges."), this->contentWidget_));
        return;
    }
    this->contentLayout_->addWidget(gridWidget);
}

std::optional<QString> TwitchBadgePickerDialog::askForToken(
    const QString &service, const QString &command, const QString &site)
{
    // The popup deletes itself when it loses focus, which the connect window
    // takes. Keep it open while the connect window is shown.
    const auto deactivateAction = this->windowDeactivateAction;
    this->windowDeactivateAction = WindowDeactivateAction::Nothing;
    const QPointer<TwitchBadgePickerDialog> self = this;

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Connect %1").arg(service));
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        QStringLiteral(
            "1. Click \"Copy command\" and open %1 while signed in.\n"
            "2. Run the copied command in its browser console (F12).\n"
            "3. Paste the copied token below.\n\n"
            "Your %1 token stays on this device.")
            .arg(service),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *copyButton = new QPushButton(
        QStringLiteral("Copy command and open %1").arg(service), &dialog);
    QObject::connect(copyButton, &QPushButton::clicked, &dialog,
                     [command, site] {
                         crossPlatformCopy(command);
                         QDesktopServices::openUrl(QUrl(site));
                     });
    layout->addWidget(copyButton);

    auto *tokenInput = new QLineEdit(&dialog);
    tokenInput->setPlaceholderText(QStringLiteral("%1 token").arg(service));
    tokenInput->setEchoMode(QLineEdit::Password);
    layout->addWidget(tokenInput);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);

    const auto accepted = dialog.exec() == QDialog::Accepted;
    if (!self)
    {
        return std::nullopt;
    }
    this->windowDeactivateAction = deactivateAction;
    this->activateWindow();
    if (!accepted)
    {
        return std::nullopt;
    }

    auto token = tokenInput->text().trimmed();
    if (token.startsWith('"') && token.endsWith('"') && token.size() >= 2)
    {
        token = token.mid(1, token.size() - 2);
    }
    if (token.isEmpty())
    {
        this->setStatus(QStringLiteral("Paste a valid %1 token.").arg(service),
                        true);
        this->rebuildContent();
        return std::nullopt;
    }
    return token;
}

void TwitchBadgePickerDialog::connectSevenTv()
{
    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto token = this->askForToken(
        QStringLiteral("7TV"), QString::fromLatin1(SEVENTV_TOKEN_COMMAND),
        QStringLiteral("https://7tv.app"));
    if (!self || !token)
    {
        return;
    }

    this->sevenTvToken_ = *token;
    this->sevenTvTokenRead_ = true;
    Credentials::instance().set(
        QString::fromLatin1(SEVENTV_CREDENTIAL_PROVIDER),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(), *token);
    this->setStatus({});
    this->loadSevenTv(true);
}

void TwitchBadgePickerDialog::disconnectSevenTv()
{
    Credentials::instance().erase(
        QString::fromLatin1(SEVENTV_CREDENTIAL_PROVIDER),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId());
    this->sevenTvToken_.clear();
    this->sevenTvLoaded_ = false;
    this->sevenTvBadges_.clear();
    this->sevenTvPaints_.clear();
    this->setStatus({});
    this->rebuildContent();
}

void TwitchBadgePickerDialog::selectSevenTv(bool paint, const QString &id)
{
    if (this->sevenTvUserId_.isEmpty())
    {
        return;
    }

    auto &active =
        paint ? this->sevenTvActivePaint_ : this->sevenTvActiveBadge_;
    const auto previous = active;
    active = id;
    this->actionInFlight_ = true;
    this->rebuildContent();

    const auto value =
        id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id);
    const QPointer<TwitchBadgePickerDialog> self = this;
    this->sevenTvRequest(
        QString::fromLatin1(paint ? SEVENTV_SET_PAINT_MUTATION
                                  : SEVENTV_SET_BADGE_MUTATION),
        {{"id", this->sevenTvUserId_}, {paint ? "paintId" : "badgeId", value}},
        [self, paint, id](const QJsonObject &) {
            self->actionInFlight_ = false;
            QString name;
            if (paint)
            {
                for (const auto &p : std::as_const(self->sevenTvPaints_))
                {
                    if (p.id == id)
                    {
                        name = p.name;
                    }
                }
            }
            else
            {
                for (const auto &b : std::as_const(self->sevenTvBadges_))
                {
                    if (b.id == id)
                    {
                        name = b.title;
                    }
                }
            }
            const auto what =
                paint ? QStringLiteral("paint") : QStringLiteral("badge");
            if (id.isEmpty())
            {
                self->channel_->addSystemMessage(
                    QStringLiteral("7TV %1 removed.").arg(what));
            }
            else
            {
                self->channel_->addSystemMessage(
                    QStringLiteral("7TV %1 set to: %2").arg(what, name));
            }
            self->rebuildContent();
        },
        [self, paint, previous](const QString &error) {
            self->actionInFlight_ = false;
            (paint ? self->sevenTvActivePaint_ : self->sevenTvActiveBadge_) =
                previous;
            self->setStatus(error, true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::bttvRequest(
    const QString &path, const std::optional<QJsonObject> &patch,
    const std::function<void(const QJsonObject &)> &onData,
    const std::function<void(const QString &)> &onError)
{
    const QPointer<TwitchBadgePickerDialog> self = this;
    auto request =
        NetworkRequest(
            QUrl(QString::fromLatin1(BTTV_API_URL) + path),
            patch ? NetworkRequestType::Patch : NetworkRequestType::Get)
            .header("Authorization", "Bearer " + this->bttvToken_.toUtf8())
            .header("Accept", "application/json")
            .timeout(15000);
    if (patch)
    {
        request = std::move(request).json(*patch);
    }
    std::move(request)
        .onSuccess([self, onData](const NetworkResult &result) {
            if (self)
            {
                onData(result.parseJson());
            }
        })
        .onError([self, onError](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            if (result.status() == 401)
            {
                onError(QStringLiteral(
                    "Your BetterTTV connection expired. Connect it again."));
                return;
            }
            const auto message = result.parseJson().value("message").toString();
            onError(message.isEmpty() ? result.formatError() : message);
        })
        .execute();
}

void TwitchBadgePickerDialog::loadBttv(bool force)
{
    BttvUsernameEffects::instance().refresh();

    if (this->bttvLoading_ || (this->bttvLoaded_ && !force))
    {
        return;
    }

    const auto twitchUserId =
        getApp()->getAccounts()->twitch.getCurrent()->getUserId();

    // The token is read from the credential store once.
    if (!this->bttvTokenRead_)
    {
        this->bttvLoading_ = true;
        this->setStatus("Loading BetterTTV...");
        this->rebuildContent();
        Credentials::instance().get(
            QString::fromLatin1(BTTV_CREDENTIAL_PROVIDER), twitchUserId, this,
            [this](const QString &token) {
                this->bttvTokenRead_ = true;
                this->bttvToken_ = token;
                this->bttvLoading_ = false;
                this->setStatus({});
                this->loadBttv(true);
            });
        return;
    }

    if (this->bttvToken_.isEmpty())
    {
        this->rebuildContent();
        return;
    }

    this->bttvLoading_ = true;
    this->setStatus("Loading BetterTTV badges...");
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto fail = [self](const QString &error) {
        self->bttvLoading_ = false;
        self->setStatus(error, true);
        self->rebuildContent();
    };
    this->bttvRequest(
        QStringLiteral("account"), std::nullopt,
        [self, twitchUserId, fail](const QJsonObject &account) {
            // Only use a BetterTTV account that belongs to the current
            // Twitch account.
            const auto providerId = account.value("providerId").toString();
            if (!providerId.isEmpty() && providerId != twitchUserId)
            {
                fail(QStringLiteral("This BetterTTV connection belongs to a "
                                    "different Twitch account."));
                return;
            }
            self->bttvBadgeShown_ = account.value("subscriptionBadge").toBool();
            self->bttvBadgeId_ =
                account.value("subscriptionBadgeId").toString();
            self->bttvEffect_ = account.value("usernameEffect").toString();
            BttvUsernameEffects::instance().setUserEffect(
                getApp()->getAccounts()->twitch.getCurrent()->getUserName(),
                self->bttvEffect_);

            self->bttvRequest(
                QString::fromLatin1(BTTV_BADGE_PATH) +
                    QStringLiteral("/eligibility"),
                std::nullopt,
                [self, fail](const QJsonObject &eligibility) {
                    // They come with the latest one first.
                    self->bttvBadges_.clear();
                    const auto list =
                        eligibility.value("eligibleBadges").toArray();
                    for (const auto &value : list)
                    {
                        const auto badge = value.toObject();
                        const auto id = badge.value("badgeId").toString();
                        const auto url = badge.value("badgeUrl").toString();
                        if (id.isEmpty() || url.isEmpty())
                        {
                            continue;
                        }
                        GqlBadge tileBadge;
                        tileBadge.id = id;
                        tileBadge.setID = id;
                        tileBadge.image1x = url;
                        tileBadge.image2x = url;
                        tileBadge.image4x = url;
                        self->bttvBadges_.push_front(tileBadge);
                    }
                    for (qsizetype i = 0; i < self->bttvBadges_.size(); ++i)
                    {
                        self->bttvBadges_[i].title =
                            QStringLiteral("BetterTTV Pro badge %1").arg(i + 1);
                    }

                    const auto next = eligibility.value("nextBadgeUnlocksAt");
                    self->bttvNextUnlock_ =
                        next.isDouble()
                            ? QDateTime::fromMSecsSinceEpoch(
                                  static_cast<qint64>(next.toDouble()))
                            : QDateTime::fromString(next.toString(),
                                                    Qt::ISODateWithMs);

                    self->bttvRequest(
                        QString::fromLatin1(BTTV_EFFECT_PATH) +
                            QStringLiteral("/eligibility"),
                        std::nullopt,
                        [self](const QJsonObject &effects) {
                            self->bttvEffectEligibility_ = effects;
                            self->bttvLoading_ = false;
                            self->bttvLoaded_ = true;
                            self->setStatus({});
                            self->rebuildContent();
                        },
                        fail);
                },
                fail);
        },
        fail);
}

void TwitchBadgePickerDialog::rebuildBttv()
{
    const bool effects = this->view_ == View::BttvEffects;
    const bool connected = !this->bttvToken_.isEmpty();

    // Title, hint and the connect button, like for 7TV.
    auto *headerRow = new QWidget(this->contentWidget_);
    auto *header = new QHBoxLayout(headerRow);
    header->setContentsMargins(0, 0, 0, 0);
    auto *texts = new QVBoxLayout();
    texts->setSpacing(0);
    auto *title = new QLabel(
        effects ? "BetterTTV username effect" : "BetterTTV Pro badge",
        this->contentWidget_);
    title->setObjectName("TwitchBadgePickerSectionLabel");
    texts->addWidget(title);
    QString hintText;
    if (connected)
    {
        hintText = effects ? "Choose how BetterTTV users see your name."
                           : "Choose one of your BetterTTV Pro badges.";
    }
    else
    {
        hintText = effects ? "Connect BetterTTV to choose an effect."
                           : "Connect BetterTTV to choose a badge.";
    }
    auto *hint = new QLabel(hintText, this->contentWidget_);
    hint->setWordWrap(true);
    texts->addWidget(hint);
    header->addLayout(texts, 1);
    auto *connectButton =
        new QPushButton(connected ? "Disconnect" : "Connect", headerRow);
    connectButton->setCursor(Qt::PointingHandCursor);
    connectButton->setEnabled(!this->bttvLoading_ && !this->actionInFlight_);
    QObject::connect(connectButton, &QPushButton::clicked, this,
                     [this, connected] {
                         // Later: both rebuild the content, which deletes this
                         // button.
                         QTimer::singleShot(0, this, [this, connected] {
                             if (connected)
                             {
                                 this->disconnectBttv();
                             }
                             else
                             {
                                 this->connectBttv();
                             }
                         });
                     });
    header->addWidget(connectButton, 0, Qt::AlignTop);
    this->contentLayout_->addWidget(headerRow);

    if (!connected || this->bttvLoading_ || !this->bttvLoaded_)
    {
        return;
    }

    if (effects)
    {
        // Your name with each effect, two per row.
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        const auto userName = user->getUserName();
        auto userColor = user->color();
        if (!userColor.isValid())
        {
            userColor = this->theme->window.text;
        }

        const auto needle = this->searchQuery_.trimmed();
        auto *list = new QWidget(this->contentWidget_);
        auto *effectGrid = new QGridLayout(list);
        effectGrid->setContentsMargins(0, 0, 0, 0);
        effectGrid->setSpacing(BADGE_GRID_SPACING);
        constexpr int effectColumns = 2;
        int effectIndex = 0;
        const auto addEffect = [&](const BttvUsernameEffect *effect) {
            const auto id = effect == nullptr ? QString{} : effect->id;
            auto *tile =
                new BttvEffectTileButton(effect, userName, userColor, list);
            tile->setSelected(id == this->bttvEffect_);
            if (effect == nullptr ||
                this->bttvEffectEligibility_.value(id).toBool())
            {
                tile->setEnabled(!this->actionInFlight_);
                QObject::connect(tile, &QPushButton::clicked, this, [this, id] {
                    this->selectBttvEffect(id);
                });
            }
            else
            {
                // Not unlocked: shown dimmed, with what unlocks it.
                tile->setEnabled(false);
                tile->setToolTip(effect->requirement.isEmpty()
                                     ? QStringLiteral("Locked")
                                     : effect->requirement);
                tile->setLocked(true);
            }
            effectGrid->addWidget(tile, effectIndex / effectColumns,
                                  effectIndex % effectColumns);
            ++effectIndex;
        };
        if (needle.isEmpty())
        {
            addEffect(nullptr);
        }
        // What BetterTTV lists for the account and the page doesn't
        // describe comes last, plain.
        auto catalog = BttvUsernameEffects::instance().effects();
        for (auto it = this->bttvEffectEligibility_.constBegin();
             it != this->bttvEffectEligibility_.constEnd(); ++it)
        {
            const auto id = it.key();
            if (it.value().isBool() &&
                std::ranges::find(catalog, id, &BttvUsernameEffect::id) ==
                    catalog.end())
            {
                catalog.push_back({
                    .id = id,
                    .label = BttvUsernameEffects::labelFromId(id),
                    .requirement = {},
                    .texture = {},
                    .outline = {},
                });
            }
        }
        for (const auto &effect : std::as_const(catalog))
        {
            if (effect.label.contains(needle, Qt::CaseInsensitive))
            {
                addEffect(&effect);
            }
        }
        if (effectIndex == 0)
        {
            delete list;
            this->contentLayout_->addWidget(makeEmptyListLabel(
                QStringLiteral("No matching effects."), this->contentWidget_));
            return;
        }
        this->contentLayout_->addWidget(list);
        return;
    }

    if (this->bttvBadges_.isEmpty())
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("This account has no BetterTTV Pro badges."),
            this->contentWidget_));
        return;
    }

    const auto latestId = this->bttvBadges_.constLast().id;
    const bool usesLatest =
        this->bttvBadgeShown_ && this->bttvBadgeId_.isEmpty();
    const auto scale = this->scale();
    auto *latestButton = new BadgePickerToggleSwitch(this->contentWidget_);
    latestButton->setChecked(usesLatest);
    latestButton->setEnabled(!this->actionInFlight_);
    latestButton->updateMetrics(scale);
    QObject::connect(latestButton, &QPushButton::clicked, this,
                     [this, latestId](bool checked) {
                         this->selectBttv(true, checked ? QString{} : latestId);
                     });
    this->contentLayout_->addWidget(
        makeSettingRow("Always use your latest badge", latestButton,
                       this->contentWidget_, scale));

    const int gridColumns = this->badgeGridColumns();
    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
    noBadgeTile->setToolTip("No BetterTTV badge");
    noBadgeTile->setEnabled(!this->actionInFlight_);
    noBadgeTile->setSelected(!this->bttvBadgeShown_);
    QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
        this->selectBttv(false, {});
    });
    grid->addWidget(noBadgeTile, 0, 0);

    const auto shownId = usesLatest ? latestId : this->bttvBadgeId_;
    int index = 1;
    for (const auto &badge : std::as_const(this->bttvBadges_))
    {
        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);
        tile->setSelected(this->bttvBadgeShown_ && badge.id == shownId);
        QObject::connect(tile, &QPushButton::clicked, this,
                         [this, id = badge.id] {
                             this->selectBttv(true, id);
                         });
        grid->addWidget(tile, index / gridColumns, index % gridColumns);
        ++index;
    }
    this->contentLayout_->addWidget(gridWidget);

    if (this->bttvNextUnlock_.isValid())
    {
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("Next badge unlocks on %1.")
                .arg(QLocale().toString(
                    this->bttvNextUnlock_.toLocalTime().date(),
                    QLocale::ShortFormat)),
            this->contentWidget_));
    }
}

void TwitchBadgePickerDialog::connectBttv()
{
    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto token = this->askForToken(
        QStringLiteral("BetterTTV"), QString::fromLatin1(BTTV_TOKEN_COMMAND),
        QStringLiteral("https://betterttv.com/dashboard/pro"));
    if (!self || !token)
    {
        return;
    }

    this->bttvToken_ = *token;
    this->bttvTokenRead_ = true;
    Credentials::instance().set(
        QString::fromLatin1(BTTV_CREDENTIAL_PROVIDER),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(), *token);
    this->setStatus({});
    this->loadBttv(true);
}

void TwitchBadgePickerDialog::disconnectBttv()
{
    Credentials::instance().erase(
        QString::fromLatin1(BTTV_CREDENTIAL_PROVIDER),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId());
    this->bttvToken_.clear();
    this->bttvLoaded_ = false;
    this->bttvBadges_.clear();
    this->bttvEffect_.clear();
    this->bttvEffectEligibility_ = {};
    this->setStatus({});
    this->rebuildContent();
}

void TwitchBadgePickerDialog::selectBttvEffect(const QString &effect)
{
    if (this->actionInFlight_ || effect == this->bttvEffect_)
    {
        return;
    }

    const auto previous = this->bttvEffect_;
    this->bttvEffect_ = effect;
    this->actionInFlight_ = true;
    this->setStatus({});
    this->rebuildContent();

    auto label = BttvUsernameEffects::labelFromId(effect);
    const auto catalog = BttvUsernameEffects::instance().effects();
    for (const auto &known : catalog)
    {
        if (effect == known.id)
        {
            label = known.label;
        }
    }

    const QPointer<TwitchBadgePickerDialog> self = this;
    this->bttvRequest(
        QString::fromLatin1(BTTV_EFFECT_PATH),
        QJsonObject{{"effect", effect.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                : QJsonValue(effect)}},
        [self, label, effect](const QJsonObject &) {
            self->actionInFlight_ = false;
            BttvUsernameEffects::instance().setUserEffect(
                getApp()->getAccounts()->twitch.getCurrent()->getUserName(),
                effect);
            self->channel_->addSystemMessage(
                label.isEmpty()
                    ? QStringLiteral("BetterTTV username effect removed.")
                    : QStringLiteral("BetterTTV username effect set to: %1")
                          .arg(label));
            self->rebuildContent();
        },
        [self, previous](const QString &error) {
            self->actionInFlight_ = false;
            self->bttvEffect_ = previous;
            self->setStatus(error, true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::selectBttv(bool show, const QString &badgeId)
{
    if (this->actionInFlight_)
    {
        return;
    }

    const bool previousShown = this->bttvBadgeShown_;
    const auto previousId = this->bttvBadgeId_;
    const bool idChanges = show && badgeId != previousId;
    const bool shownChanges = show != previousShown;
    if (!idChanges && !shownChanges)
    {
        return;
    }

    this->bttvBadgeShown_ = show;
    if (show)
    {
        this->bttvBadgeId_ = badgeId;
    }
    this->actionInFlight_ = true;
    this->setStatus({});
    this->rebuildContent();

    const QPointer<TwitchBadgePickerDialog> self = this;
    const auto path = QString::fromLatin1(BTTV_BADGE_PATH);
    const std::function<void()> done = [self, show, badgeId] {
        self->actionInFlight_ = false;
        QString message;
        if (!show)
        {
            message = QStringLiteral("BetterTTV badge cleared.");
        }
        else if (badgeId.isEmpty())
        {
            message =
                QStringLiteral("BetterTTV badge set to: always the latest");
        }
        else
        {
            QString title;
            for (const auto &badge : std::as_const(self->bttvBadges_))
            {
                if (badge.id == badgeId)
                {
                    title = badge.title;
                }
            }
            message = QStringLiteral("BetterTTV badge set to: %1").arg(title);
        }
        self->channel_->addSystemMessage(message);
        self->rebuildContent();
    };
    const std::function<void(const QString &)> fail =
        [self, previousShown, previousId](const QString &error) {
            self->actionInFlight_ = false;
            self->bttvBadgeShown_ = previousShown;
            self->bttvBadgeId_ = previousId;
            self->setStatus(error, true);
            self->rebuildContent();
        };
    // Which badge and whether it's shown are set one after the other.
    const std::function<void()> setShown = [self, path, show, done, fail] {
        self->bttvRequest(
            path, QJsonObject{{"badge", show}},
            [done](const QJsonObject &) {
                done();
            },
            fail);
    };

    if (!idChanges)
    {
        setShown();
        return;
    }
    this->bttvRequest(
        path,
        QJsonObject{{"badgeId", badgeId.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                  : QJsonValue(badgeId)}},
        [shownChanges, setShown, done](const QJsonObject &) {
            if (shownChanges)
            {
                setShown();
            }
            else
            {
                done();
            }
        },
        fail);
}

QString TwitchBadgePickerDialog::authTokenOrMessage()
{
    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (auth.hasToken())
        return auth.token;

    const auto message =
        authError.isEmpty()
            ? MoltorinoAuth::authRequiredMessage("using badge picker")
            : authError;
    this->setStatus(message, true);
    return {};
}

}  // namespace chatterino
