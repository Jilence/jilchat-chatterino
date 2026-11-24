// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/RoundedStyle.hpp"

#include "widgets/helper/LiquidGlass.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include <QWidget>

#ifdef USEWINSDK
// clang-format off
#    include <Windows.h>
#    include <dwmapi.h>
// clang-format on
#    pragma comment(lib, "Dwmapi.lib")
#endif

namespace chatterino {

namespace {

constexpr qreal FIELD_RADIUS = 7;
constexpr qreal MENU_ITEM_RADIUS = 5;
constexpr qreal CHECKBOX_RADIUS = 4;

/// Size of a check box drawn as a switch.
constexpr int SWITCH_WIDTH = 32;
constexpr int SWITCH_HEIGHT = 18;

/// Thickness of a scroll bar; the visible thumb is thinner.
constexpr int SCROLLBAR_EXTENT = 10;

bool isDark(const QColor &color)
{
    return color.lightness() < 128;
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t),
                            float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

void fillRounded(QPainter *painter, const QRectF &rect, qreal radius,
                 const QBrush &fill, const QColor &border)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(border.isValid() ? QPen(border, 1.0) : QPen(Qt::NoPen));
    painter->setBrush(fill);
    painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius,
                             radius);
    painter->restore();
}

enum class Surface {
    /// A button or pop-up button: glass that sits on the window.
    Raised,
    /// A text field: a recessed well.
    Field,
    /// The default button: filled with the accent colour.
    Accent,
};

struct SurfaceState {
    bool hover = false;
    bool pressed = false;
    bool focus = false;
    bool enabled = true;
};

/// A quiet glass surface the way macOS draws controls in dark mode: barely
/// lighter than the window, a hairline highlight along the top edge where
/// the light hits, a faint outline and a soft accent ring on focus. Light
/// themes get white glass with a fine shadow instead.
void paintSurface(QPainter *painter, const QRectF &bounds, qreal radius,
                  const QPalette &palette, Surface surface,
                  const SurfaceState &state)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor accent = palette.color(QPalette::Highlight);
    const bool dark = isDark(window);
    const QRectF rect = bounds.adjusted(1, 1, -1, -1);
    radius = std::min({radius, rect.height() / 2, rect.width() / 2});

    QColor fill;
    switch (surface)
    {
        case Surface::Raised:
            fill = dark ? mix(window, Qt::white, 0.105) : QColor(Qt::white);
            if (state.pressed)
            {
                fill =
                    dark ? mix(window, Qt::white, 0.19) : QColor(232, 232, 236);
            }
            else if (state.hover)
            {
                fill = dark ? mix(window, Qt::white, 0.145)
                            : QColor(246, 246, 248);
            }
            break;
        case Surface::Field:
            fill = dark ? mix(window, Qt::white, 0.045) : QColor(Qt::white);
            if (state.hover && !state.focus)
            {
                fill = dark ? mix(window, Qt::white, 0.065) : fill;
            }
            break;
        case Surface::Accent:
            fill = state.pressed ? accent.darker(115)
                                 : (state.hover ? accent.lighter(108) : accent);
            break;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (!state.enabled)
    {
        painter->setOpacity(0.5);
    }

    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);

    // Soft shadow under raised controls (very faint in dark mode).
    if (surface != Surface::Field && !state.pressed)
    {
        QPainterPath shadow;
        shadow.addRoundedRect(rect.translated(0, 0.75), radius, radius);
        painter->fillPath(shadow, QColor(0, 0, 0, dark ? 70 : 28));
    }

    painter->fillPath(path, fill);

    // Hairline highlight where the light hits the top edge of the glass.
    if (surface != Surface::Field)
    {
        painter->save();
        painter->setClipPath(path);
        QLinearGradient top(rect.topLeft(),
                            QPointF(rect.left(), rect.top() + 3));
        top.setColorAt(
            0, QColor(255, 255, 255,
                      surface == Surface::Accent ? 70 : (dark ? 30 : 0)));
        top.setColorAt(1, QColor(255, 255, 255, 0));
        painter->fillRect(QRectF(rect.left(), rect.top(), rect.width(), 3),
                          top);
        painter->restore();
    }

    // Outline: a faint edge, slightly brighter at the top in dark mode.
    QLinearGradient edge(rect.topLeft(), rect.bottomLeft());
    if (dark)
    {
        const int topAlpha = surface == Surface::Field ? 22 : 34;
        const int bottomAlpha = surface == Surface::Field ? 30 : 10;
        edge.setColorAt(0, QColor(255, 255, 255, topAlpha));
        edge.setColorAt(1, QColor(255, 255, 255, bottomAlpha));
    }
    else
    {
        edge.setColorAt(0,
                        QColor(0, 0, 0, surface == Surface::Field ? 45 : 22));
        edge.setColorAt(1,
                        QColor(0, 0, 0, surface == Surface::Field ? 30 : 40));
    }
    if (surface != Surface::Accent)
    {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QBrush(edge), 1.0));
        painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5),
                                 radius - 0.5, radius - 0.5);
    }

    // macOS style focus ring: a soft accent band around the control.
    if (state.focus)
    {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(withAlpha(accent, 140), 2.5));
        painter->drawRoundedRect(rect.adjusted(-1.25, -1.25, 1.25, 1.25),
                                 radius + 1.25, radius + 1.25);
    }

    painter->restore();
}

/// A switch: a capsule track (accent when on) and a knob of frosted glass.
void paintSwitch(QPainter *painter, const QRectF &bounds,
                 const QPalette &palette, bool on, bool partial, bool enabled,
                 bool hover, bool focus)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor accent = palette.color(QPalette::Highlight);
    const bool dark = isDark(window);
    const QRectF track =
        QRectF(0, 0, SWITCH_WIDTH - 2, SWITCH_HEIGHT - 2)
            .translated(bounds.left() + 1,
                        bounds.center().y() - (SWITCH_HEIGHT - 2) / 2.0);
    const qreal radius = track.height() / 2;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (!enabled)
    {
        painter->setOpacity(0.45);
    }

    QColor trackColor =
        on ? accent
           : (dark ? mix(window, Qt::white, 0.17) : QColor(220, 220, 225));
    if (hover)
    {
        trackColor = dark ? trackColor.lighter(110) : trackColor.darker(103);
    }
    QPainterPath trackPath;
    trackPath.addRoundedRect(track, radius, radius);
    painter->fillPath(trackPath, trackColor);
    // Recessed: a little shade along the top inside the track.
    painter->save();
    painter->setClipPath(trackPath);
    QLinearGradient shade(track.topLeft(),
                          QPointF(track.left(), track.top() + 4));
    shade.setColorAt(0, QColor(0, 0, 0, 45));
    shade.setColorAt(1, QColor(0, 0, 0, 0));
    painter->fillRect(track, shade);
    painter->restore();

    if (focus)
    {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(withAlpha(accent, 140), 2.5));
        painter->drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5),
                                 radius + 1.5, radius + 1.5);
    }

    // Knob.
    const qreal inset = 1.5;
    const qreal d = track.height() - 2 * inset;
    const qreal travel = track.width() - 2 * inset - d;
    const qreal x =
        track.left() + inset + (partial ? travel / 2 : (on ? travel : 0));
    const QRectF knob(x, track.top() + inset, d, d);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 55));
    painter->drawEllipse(knob.translated(0, 0.6));

    // Frosted glass: not pure white, lit from the top left.
    const QColor knobColor = dark ? QColor(222, 222, 226) : QColor(Qt::white);
    QRadialGradient bead(knob.center() - QPointF(d * 0.2, d * 0.25), d * 0.8);
    bead.setColorAt(0, knobColor.lighter(106));
    bead.setColorAt(1, knobColor.darker(dark ? 106 : 103));
    painter->setBrush(bead);
    painter->drawEllipse(knob);

    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(255, 255, 255, dark ? 140 : 200), 0.8));
    painter->drawArc(knob.adjusted(0.6, 0.6, -0.6, -0.6), 40 * 16, 100 * 16);

    painter->restore();
}

void paintChevron(QPainter *painter, const QRectF &area, const QColor &color)
{
    const qreal w = std::min(area.width(), area.height()) * 0.42;
    const QPointF c = area.center() + QPointF(0, w * 0.12);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(
        QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    QPainterPath chevron;
    chevron.moveTo(c + QPointF(-w / 2, -w / 4));
    chevron.lineTo(c + QPointF(0, w / 4));
    chevron.lineTo(c + QPointF(w / 2, -w / 4));
    painter->drawPath(chevron);
    painter->restore();
}

bool isSwitch(const QWidget *widget)
{
    return qobject_cast<const QCheckBox *>(widget) != nullptr;
}

SurfaceState stateOf(const QStyleOption *option)
{
    SurfaceState state;
    state.enabled = (option->state & QStyle::State_Enabled) != 0;
    state.hover =
        (option->state & QStyle::State_MouseOver) != 0 && state.enabled;
    state.pressed =
        (option->state & (QStyle::State_Sunken | QStyle::State_On)) != 0;
    state.focus = (option->state & QStyle::State_HasFocus) != 0 &&
                  (option->state & QStyle::State_KeyboardFocusChange) != 0 &&
                  state.enabled;
    return state;
}

}  // namespace

RoundedStyle::RoundedStyle(QStyle *base)
    : QProxyStyle(base)
{
}

void RoundedStyle::drawPrimitive(PrimitiveElement element,
                                 const QStyleOption *option, QPainter *painter,
                                 const QWidget *widget) const
{
    const QRectF rect = option->rect;
    const auto &palette = option->palette;
    const bool enabled = (option->state & State_Enabled) != 0;
    const bool hover = (option->state & State_MouseOver) != 0 && enabled;
    const bool focus = (option->state & State_HasFocus) != 0 && enabled;
    const QColor accent = palette.color(QPalette::Highlight);

    switch (element)
    {
        case PE_PanelButtonCommand: {
            const auto *button =
                qstyleoption_cast<const QStyleOptionButton *>(option);
            const bool isDefault =
                button != nullptr &&
                (button->features & QStyleOptionButton::DefaultButton) != 0;
            // Capsules, like push buttons on macOS 26.
            paintSurface(painter, rect, rect.height() / 2, palette,
                         isDefault ? Surface::Accent : Surface::Raised,
                         stateOf(option));
            return;
        }

        case PE_PanelLineEdit: {
            const auto *frame =
                qstyleoption_cast<const QStyleOptionFrame *>(option);
            if (frame != nullptr && frame->lineWidth <= 0)
            {
                // Frameless line edits (e.g. inside other controls).
                painter->fillRect(option->rect, palette.brush(QPalette::Base));
                return;
            }
            SurfaceState state = stateOf(option);
            state.pressed = false;
            state.focus = focus;  // text fields always show their focus
            paintSurface(painter, rect, FIELD_RADIUS, palette, Surface::Field,
                         state);
            return;
        }

        case PE_FrameLineEdit:
            // Drawn with the panel above.
            return;

        case PE_IndicatorCheckBox: {
            const bool on = (option->state & State_On) != 0;
            const bool partial = (option->state & State_NoChange) != 0;
            if (isSwitch(widget))
            {
                paintSwitch(
                    painter, rect, palette, on, partial, enabled, hover,
                    focus && (option->state & State_KeyboardFocusChange) != 0);
                return;
            }

            // Check boxes in lists and menus stay boxes, just rounder.
            const QRectF box = rect.adjusted(1, 1, -1, -1);
            if (on || partial)
            {
                fillRounded(painter, box, CHECKBOX_RADIUS,
                            enabled ? accent : withAlpha(accent, 110),
                            QColor());
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(
                    QPen(Qt::white, std::max(1.5, box.width() / 7.0),
                         Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                if (partial)
                {
                    painter->drawLine(QPointF(box.left() + box.width() * 0.28,
                                              box.center().y()),
                                      QPointF(box.right() - box.width() * 0.28,
                                              box.center().y()));
                }
                else
                {
                    QPainterPath check;
                    check.moveTo(box.left() + box.width() * 0.25,
                                 box.top() + box.height() * 0.52);
                    check.lineTo(box.left() + box.width() * 0.43,
                                 box.top() + box.height() * 0.70);
                    check.lineTo(box.left() + box.width() * 0.76,
                                 box.top() + box.height() * 0.32);
                    painter->drawPath(check);
                }
                painter->restore();
            }
            else
            {
                SurfaceState state = stateOf(option);
                state.pressed = false;
                state.focus = false;
                paintSurface(painter, rect, CHECKBOX_RADIUS, palette,
                             Surface::Field, state);
            }
            return;
        }

#ifdef USEWINSDK
        // Windows 11 rounds the menu window itself (installRoundedWindows)
        // and draws its border, so a square frame would only cut the corners.
        case PE_FrameMenu:
            return;
#endif

        case PE_PanelMenu: {
            // Liquid glass over the window the menu opened from: its pixels
            // are already painted, the menu only refracts them.
            auto *menu = const_cast<QWidget *>(widget);
            if (qobject_cast<QMenu *>(menu) != nullptr)
            {
                QWidget *behind = menu->parentWidget() != nullptr
                                      ? menu->parentWidget()->window()
                                      : QApplication::activeWindow();
                if (behind != nullptr && behind != menu->window())
                {
                    const QColor tint = palette.color(QPalette::Window);
                    liquidglass::paintGlassWindow(
                        *painter, menu, behind, tint, 1.0, !isDark(tint),
                        8 * menu->devicePixelRatioF());
                    return;
                }
            }
            break;
        }

        default:
            break;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void RoundedStyle::drawControl(ControlElement element,
                               const QStyleOption *option, QPainter *painter,
                               const QWidget *widget) const
{
    if (element == CE_MenuItem)
    {
        const auto *item =
            qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        const bool selected = (option->state & State_Selected) != 0;
        const bool enabled = (option->state & State_Enabled) != 0;
        if (item != nullptr && selected && enabled &&
            item->menuItemType != QStyleOptionMenuItem::Separator)
        {
            // An inset rounded pill instead of a full-width bar.
            const QRectF pill = QRectF(option->rect).adjusted(4, 1, -4, -1);
            fillRounded(painter, pill, MENU_ITEM_RADIUS,
                        option->palette.brush(QPalette::Highlight), QColor());

            QStyleOptionMenuItem plain(*item);
            plain.state &= ~State_Selected;
            const auto text = option->palette.color(QPalette::HighlightedText);
            plain.palette.setColor(QPalette::Text, text);
            plain.palette.setColor(QPalette::ButtonText, text);
            plain.palette.setColor(QPalette::WindowText, text);
            QProxyStyle::drawControl(element, &plain, painter, widget);
            return;
        }
    }

    QProxyStyle::drawControl(element, option, painter, widget);
}

void RoundedStyle::drawComplexControl(ComplexControl control,
                                      const QStyleOptionComplex *option,
                                      QPainter *painter,
                                      const QWidget *widget) const
{
    if (control == CC_ComboBox)
    {
        const auto *combo =
            qstyleoption_cast<const QStyleOptionComboBox *>(option);
        if (combo != nullptr && !combo->editable)
        {
            // A pop-up button: one glass capsule with a chevron.
            SurfaceState state = stateOf(option);
            state.pressed = (option->state & State_On) != 0;
            paintSurface(painter, option->rect, FIELD_RADIUS, option->palette,
                         Surface::Raised, state);
            const QRect arrow = this->subControlRect(CC_ComboBox, option,
                                                     SC_ComboBoxArrow, widget);
            auto chevron = option->palette.color(QPalette::ButtonText);
            chevron.setAlpha(170);
            paintChevron(painter, arrow, chevron);
            return;
        }
    }

    if (control == CC_ScrollBar)
    {
        const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (bar != nullptr)
        {
            // Transparent track and a thin rounded thumb that thickens a
            // little under the mouse, like overlay scroll bars on macOS.
            const QRect slider = this->subControlRect(
                CC_ScrollBar, option, SC_ScrollBarSlider, widget);
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const bool active =
                (option->state & State_MouseOver) != 0 ||
                (bar->activeSubControls & SC_ScrollBarSlider) != 0;
            const qreal thickness = active ? 7 : 5;
            QRectF thumb = slider;
            if (horizontal)
            {
                thumb.setTop(thumb.center().y() - thickness / 2);
                thumb.setHeight(thickness);
                thumb.adjust(2, 0, -2, 0);
            }
            else
            {
                thumb.setLeft(thumb.center().x() - thickness / 2);
                thumb.setWidth(thickness);
                thumb.adjust(0, 2, 0, -2);
            }
            const bool dark = isDark(option->palette.color(QPalette::Window));
            const bool pressed = (option->state & State_Sunken) != 0 &&
                                 (bar->activeSubControls & SC_ScrollBarSlider);
            QColor color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
            color.setAlpha(pressed ? 150 : (active ? 115 : 70));
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawRoundedRect(thumb, thickness / 2, thickness / 2);
            painter->restore();
            return;
        }
    }

    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

QRect RoundedStyle::subControlRect(ComplexControl control,
                                   const QStyleOptionComplex *option,
                                   SubControl subControl,
                                   const QWidget *widget) const
{
    if (control == CC_ScrollBar)
    {
        const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (bar != nullptr)
        {
            // No arrow buttons: the whole bar is the groove.
            const QRect r = bar->rect;
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const int length = horizontal ? r.width() : r.height();
            const int range = bar->maximum - bar->minimum;
            const int minThumb = 24;
            int thumb = length;
            int pos = 0;
            if (range > 0)
            {
                thumb =
                    std::max(minThumb, int(qint64(length) * bar->pageStep /
                                           std::max(1, range + bar->pageStep)));
                thumb = std::min(thumb, length);
                pos = QStyle::sliderPositionFromValue(
                    bar->minimum, bar->maximum, bar->sliderPosition,
                    length - thumb, bar->upsideDown);
            }
            auto along = [&](int start, int size) {
                return horizontal
                           ? QRect(r.x() + start, r.y(), size, r.height())
                           : QRect(r.x(), r.y() + start, r.width(), size);
            };
            switch (subControl)
            {
                case SC_ScrollBarAddLine:
                case SC_ScrollBarSubLine:
                case SC_ScrollBarFirst:
                case SC_ScrollBarLast:
                    return {};
                case SC_ScrollBarGroove:
                    return r;
                case SC_ScrollBarSlider:
                    return along(pos, thumb);
                case SC_ScrollBarSubPage:
                    return along(0, pos);
                case SC_ScrollBarAddPage:
                    return along(pos + thumb, length - pos - thumb);
                default:
                    break;
            }
        }
    }

    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

int RoundedStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                              const QWidget *widget) const
{
    switch (metric)
    {
        // Room around the items so the selection pill floats inside the menu.
        case PM_MenuVMargin:
            return 5;
        case PM_MenuHMargin:
            return 0;
#ifdef USEWINSDK
        case PM_MenuPanelWidth:
            return 0;
#endif
        case PM_ScrollBarExtent:
            return SCROLLBAR_EXTENT;
        case PM_ScrollBarSliderMin:
            return 24;
        case PM_IndicatorWidth:
            if (isSwitch(widget))
            {
                return SWITCH_WIDTH;
            }
            break;
        case PM_IndicatorHeight:
            if (isSwitch(widget))
            {
                return SWITCH_HEIGHT;
            }
            break;
        case PM_CheckBoxLabelSpacing:
            if (isSwitch(widget))
            {
                return 8;
            }
            break;
        default:
            break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

namespace {

#ifdef USEWINSDK
// Not in older SDKs.
constexpr DWORD CORNER_PREFERENCE_ATTRIBUTE =
    33;                                // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int CORNER_ROUND = 2;        // DWMWCP_ROUND
constexpr int CORNER_ROUND_SMALL = 3;  // DWMWCP_ROUNDSMALL

class RoundedWindowFilter : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
        {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget != nullptr && widget->isWindow())
            {
                this->round(widget);
            }
        }
        return false;
    }

private:
    void round(QWidget *window)
    {
        // A window can get a new native handle (e.g. new window flags).
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        const auto done = window->property("_chatterinoRoundedHwnd");
        if (done.isValid() && done.toULongLong() == quintptr(hwnd))
        {
            return;
        }
        window->setProperty("_chatterinoRoundedHwnd", quintptr(hwnd));

        const bool isTooltip = window->inherits("QTipLabel") ||
                               window->windowType() == Qt::ToolTip;
        const int preference = isTooltip ? CORNER_ROUND_SMALL : CORNER_ROUND;
        // Fails harmlessly before Windows 11.
        DwmSetWindowAttribute(hwnd, CORNER_PREFERENCE_ATTRIBUTE, &preference,
                              sizeof(preference));
    }
};
#endif

}  // namespace

void installRoundedWindows()
{
#ifdef USEWINSDK
    qApp->installEventFilter(new RoundedWindowFilter(qApp));
#endif
}

}  // namespace chatterino
