// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageElement.hpp"

#include <QColor>
#include <QPainter>
#include <QPoint>

#include <optional>

namespace pajlada::Signals {
class SignalHolder;
}  // namespace pajlada::Signals

namespace chatterino {

class ColorProvider;
class Theme;
class Settings;
class Channel;
struct Selection;
struct Message;

// TODO: Figure out if this could be a subset of Theme instead (e.g. Theme::MessageColors)
struct MessageColors {
    QColor channelBackground;

    // true if any of the background colors have transparency
    bool hasTransparency = false;

    QColor regularBg;
    QColor alternateBg;

    QColor disabled;
    QColor selection;

    QColor regularText;
    QColor linkText;
    QColor systemText;
    QColor timestampText;

    QColor messageSeperator;

    QColor focusedLastMessageLine;
    QColor unfocusedLastMessageLine;

    void applyTheme(Theme *theme, bool isOverlay, int backgroundOpacity);
};

// TODO: Explore if we can let settings own this
struct MessagePreferences {
    QColor lastMessageColor;
    Qt::BrushStyle lastMessagePattern{};

    bool enableRedeemedHighlight{};
    bool enableFirstMessageHighlight{};
    bool enableSubHighlight{};
    bool enableFollowHighlight{};
    bool enableWatchStreakHighlight{};
    bool enableAutomodHighlight{};
    bool enableAnnouncementHighlight{};
    bool enableColoredAnnouncementHighlight{};
    bool enableClientDetectionHighlight{};
    bool multipleHighlightBands{};

    bool alternateMessages{};
    bool separateMessages{};

    bool fadeMessageHistory{};

    QColor clientDetectionWebColor;
    QColor clientDetectionAndroidColor;
    QColor clientDetectionIosColor;

    void connectSettings(Settings *settings,
                         pajlada::Signals::SignalHolder &holder);
};

/// The theme's text shadow as it is drawn behind animated elements, scaled
/// to the message.
struct AnimatedMessageShadow {
    QColor color{Qt::black};
    /// 0 to 1
    qreal opacity{};
    QPointF offset;
    qreal blurRadius{};
    /// Whether emotes, emoji and cheermotes get the shadow too.
    bool emotes = true;
};

struct MessagePaintContext {
    QPainter &painter;
    const Selection &selection;
    const ColorProvider &colorProvider;
    const MessageColors &messageColors;
    const MessagePreferences &preferences;

    // width of the area we have to draw on
    const int canvasWidth{};
    // whether the painting should be treated as if this view's window is focused
    const bool isWindowFocused{};
    // whether the painting should be treated as if this view is the special mentions view
    const bool isMentions{};

    // y coordinate we're currently painting at
    int y{};

    // Index of the message that is currently being painted
    // This index refers to the snapshot being used in the painting
    size_t messageIndex{};

    bool isLastReadMessage{};
    bool isCollapsed{};

    bool tintByPlatform{};

    // A shadow behind text and emotes, as the theme asks for it.
    bool paintMessageShadow{};
    bool messageShadowEmotes = true;
    QColor messageShadowColor{Qt::black};
    // In percent.
    int messageShadowOpacity{};
    QPoint messageShadowOffset{1, 1};
    int messageShadowBlur{};
    // Added to the opacity of highlight colors, in percent points.
    int highlightOpacityAdjustment{};
};

struct MessageLayoutContext {
    struct MessageTextRange {
        qsizetype start{};
        qsizetype length{};
    };

    const MessageColors &messageColors;
    MessageElementFlags flags;

    int width = 1;
    float scale = 1;
    float imageScale = 1;
    float emoteScale = 1;
    float badgeScale = 1;
    bool centerBadges = false;

    Channel *selectedChannel = nullptr;
    const Message &message;

    void resetMessageTextCursor() const;
    /// Finds `renderedText` in the message's text, after what was claimed
    /// before. This is how elements learn which part of the text they show.
    std::optional<MessageTextRange> claimMessageTextRange(
        QStringView renderedText) const;

    mutable qsizetype messageTextCursor_{};
};

}  // namespace chatterino
