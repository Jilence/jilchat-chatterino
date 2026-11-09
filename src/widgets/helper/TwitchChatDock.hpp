// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>

class QWidget;

namespace chatterino {

class LabelButton;
class Split;
class SplitContainer;
class Window;

/// Lets the main window be dragged onto Twitch chat in a browser. It then sits
/// exactly over that chat, follows the browser and shows the same channel.
/// Dragging it away puts it back as it was. Only does anything on Windows.
class TwitchChatDock : public QObject, public QAbstractNativeEventFilter
{
public:
    explicit TwitchChatDock(Window &window);
    ~TwitchChatDock() override;

    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

    /// Normally tabs come back a moment later, so that the page gets the
    /// window's real size. Use immediately when the layout is about to be saved.
    void undock(bool immediately = false);

private:
    /// Where a browser window currently shows Twitch chat, in screen pixels
    struct ChatLocation {
        quintptr browser = 0;
        QRect chat;
        /// The chat was found by name, so it's certain, and if it goes missing
        /// the page really isn't showing chat
        bool exact = false;
    };

    void checkDragMoved();
    void onDragMoved();
    void onDragFinished();
    void dock(const ChatLocation &location);
    void restoreTabs();
    void followBrowser();
    void placeOver(const QRect &rect);
    void keepAboveBrowser();
    void setHidden(bool hidden);
    void showChannelFromTitle();
    void selectChannel(const QString &login);
    /// The tab bar is hidden while docked, so this lists every tab to switch to
    void showTabsMenu();
    void setTabsButtonVisible(bool visible);

    void findChat(quintptr browser);
    void onChatFound(const ChatLocation &location);

    void showPreview(const QRect &rect);
    void hidePreview();

    Window &window_;
    quintptr windowHandle_ = 0;

    bool dragging_ = false;
    bool moved_ = false;
    QRect dragStartRect_;
    QTimer dragTimer_;
    quintptr dragBrowser_ = 0;
    ChatLocation dragLocation_;
    QElapsedTimer sinceDragSearch_;
    QPointer<QWidget> preview_;

    bool docked_ = false;
    bool hiddenByDock_ = false;
    bool usedExactLocation_ = false;
    quintptr browser_ = 0;
    QRect browserRect_;
    QRect chatRect_;
    QElapsedTimer sinceSearch_;
    QTimer followTimer_;
    bool searching_ = false;

    QRect restoreGeometry_;
    bool restoreMaximized_ = false;
    bool restoreShowTabs_ = true;
    QString channel_;
    QPointer<SplitContainer> addedPage_;
    QPointer<Split> addedSplit_;
    QPointer<LabelButton> tabsButton_;
    QWidget *previousPage_ = nullptr;
};

}  // namespace chatterino
