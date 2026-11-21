// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QWidget;

namespace chatterino::transitions {

/// Whether the "Smooth animations" setting is on.
bool smoothAnimationsEnabled();

/// True while pages are gliding across. Chats hold off on redrawing animated
/// emotes until it's over, so the animation gets the whole frame.
bool pageTransitionRunning();

enum class PageDirection {
    /// The new page comes from the right, or from below when vertical.
    Forward,
    /// The new page comes from the left, or from above when vertical.
    Backward,
};

/// Call just before switching from oldPage to newPage. Both pages glide and
/// fade across as pictures, so a busy chat can't make the animation stutter.
void slidePages(QWidget *oldPage, QWidget *newPage, PageDirection direction,
                bool vertical);

/// Call just before a widget changes its look or contents. The old look fades
/// away over the new one. With includeBehind the picture also has what's behind
/// the widget, for widgets that don't draw their whole background.
void fadeChange(QWidget *widget, bool includeBehind = false);

/// Fades a child widget in, such as a tab that was just added.
void fadeInWidget(QWidget *widget);

/// Fades a window in as it opens. Windows with a riseBy also glide up that far.
void fadeInWindow(QWidget *window, int riseBy = 0);

/// Call as a window closes: a picture of it stays behind and fades away, so the
/// real window can close right away without changing how closing works.
void fadeOutWindow(QWidget *window);

/// Applies the setting to menus and tooltips, now and whenever it changes.
void initializeAnimationEffects();

}  // namespace chatterino::transitions
