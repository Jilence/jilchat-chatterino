// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/TwitchChatDock.hpp"

#include "Application.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QWidget>

#ifdef USEWINSDK
// clang-format off
#    include <Windows.h>
#    include <dwmapi.h>
#    include <objbase.h>
#    include <uiautomation.h>
#    include <wrl/client.h>
// clang-format on

#    pragma comment(lib, "Dwmapi.lib")
#    pragma comment(lib, "Ole32.lib")
#    pragma comment(lib, "OleAut32.lib")
#endif

namespace chatterino {

#ifdef USEWINSDK

namespace {

/// Asks again this often while a window is being dragged over a browser
constexpr int DRAG_SEARCH_MS = 150;
/// Asks again this often while docked, in case the page layout changed
constexpr int DOCKED_SEARCH_MS = 700;
constexpr int FOLLOW_MS = 16;
constexpr int DRAG_CHECK_MS = 33;
/// Twitch's own chat column and top bar, used if the chat can't be found by
/// name, like when Twitch is in another language
constexpr int TWITCH_CHAT_WIDTH = 340;
constexpr int TWITCH_TOP_BAR = 50;

HWND toHwnd(quintptr value)
{
    return reinterpret_cast<HWND>(value);
}

QRect toQRect(const RECT &rect)
{
    return {QPoint(rect.left, rect.top),
            QSize(rect.right - rect.left, rect.bottom - rect.top)};
}

QRect windowRect(HWND hwnd)
{
    RECT rect{};
    ::GetWindowRect(hwnd, &rect);
    return toQRect(rect);
}

bool isBrowser(HWND hwnd)
{
    DWORD processId = 0;
    ::GetWindowThreadProcessId(hwnd, &processId);
    HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, processId);
    if (process == nullptr)
    {
        return false;
    }

    WCHAR path[MAX_PATH];
    DWORD length = MAX_PATH;
    const bool ok = ::QueryFullProcessImageNameW(process, 0, path, &length);
    ::CloseHandle(process);
    if (!ok)
    {
        return false;
    }

    static const QStringList browsers{
        "chrome.exe", "brave.exe",   "msedge.exe", "firefox.exe",
        "opera.exe",  "vivaldi.exe", "zen.exe",
    };
    const auto name =
        QFileInfo(QString::fromWCharArray(path, int(length))).fileName();
    return browsers.contains(name, Qt::CaseInsensitive);
}

QString windowTitle(HWND hwnd)
{
    WCHAR title[512];
    const int length = ::GetWindowTextW(hwnd, title, 512);
    return QString::fromWCharArray(title, length);
}

bool isShowingTwitch(HWND hwnd)
{
    return windowTitle(hwnd).contains(QStringLiteral("Twitch"));
}

struct TopWindowSearch {
    POINT cursor;
    HWND ignoreA;
    HWND ignoreB;
    HWND found;
};

BOOL CALLBACK findTopWindow(HWND hwnd, LPARAM param)
{
    auto *search = reinterpret_cast<TopWindowSearch *>(param);
    if (hwnd == search->ignoreA || hwnd == search->ignoreB ||
        !::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
    {
        return true;
    }
    // Click-through windows, like ping cards and game overlays, aren't really
    // what the mouse is over
    if ((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0)
    {
        return true;
    }
    int cloaked = 0;
    ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked != 0)
    {
        return true;
    }
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect) || !::PtInRect(&rect, search->cursor))
    {
        return true;
    }
    search->found = hwnd;
    return false;
}

/// The browser showing Twitch under the mouse, if that's what's there
HWND twitchBrowserUnderCursor(HWND ignoreA, HWND ignoreB)
{
    TopWindowSearch search{{}, ignoreA, ignoreB, nullptr};
    ::GetCursorPos(&search.cursor);
    ::EnumWindows(&findTopWindow, reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr || !isShowingTwitch(search.found) ||
        !isBrowser(search.found))
    {
        return nullptr;
    }
    return search.found;
}

/// Reads a browser's page layout. This asks the browser and can be slow, so it
/// never runs on the UI thread.
bool readChatRect(HWND browser, QRect &chat, bool &exact)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IUIAutomation> automation;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation))))
    {
        return false;
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(browser, &root)) || !root)
    {
        return false;
    }

    // Twitch names its chat column "Stream Chat"
    VARIANT name{};
    name.vt = VT_BSTR;
    name.bstrVal = ::SysAllocString(L"Stream Chat");
    ComPtr<IUIAutomationCondition> byName;
    const auto madeCondition =
        automation->CreatePropertyCondition(UIA_NamePropertyId, name, &byName);
    ::VariantClear(&name);
    if (FAILED(madeCondition))
    {
        return false;
    }

    ComPtr<IUIAutomationElement> element;
    if (SUCCEEDED(
            root->FindFirst(TreeScope_Descendants, byName.Get(), &element)) &&
        element)
    {
        RECT rect{};
        if (SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) &&
            rect.right > rect.left && rect.bottom > rect.top)
        {
            chat = toQRect(rect);
            exact = true;
            return true;
        }
    }

    // Otherwise work it out from the page area and Twitch's usual layout
    VARIANT documentType{};
    documentType.vt = VT_I4;
    documentType.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> byType;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   documentType, &byType)))
    {
        return false;
    }
    ComPtr<IUIAutomationElement> document;
    if (FAILED(
            root->FindFirst(TreeScope_Descendants, byType.Get(), &document)) ||
        !document)
    {
        return false;
    }
    RECT page{};
    if (FAILED(document->get_CurrentBoundingRectangle(&page)) ||
        page.right <= page.left)
    {
        return false;
    }
    const double scale = ::GetDpiForWindow(browser) / 96.0;
    const int width = int(TWITCH_CHAT_WIDTH * scale);
    const int top = page.top + int(TWITCH_TOP_BAR * scale);
    chat =
        QRect(QPoint(page.right - width, top), QSize(width, page.bottom - top));
    exact = false;
    return true;
}

/// Marks where the window will land, and lets the mouse straight through
class DockPreview : public QWidget
{
public:
    DockPreview()
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool |
                               Qt::WindowStaysOnTopHint |
                               Qt::WindowTransparentForInput |
                               Qt::WindowDoesNotAcceptFocus)
    {
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->setAttribute(Qt::WA_ShowWithoutActivating);
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.fillRect(this->rect(), QColor(227, 16, 28, 70));
        painter.setPen(QPen(QColor(255, 92, 77), 3));
        painter.drawRect(this->rect().adjusted(1, 1, -2, -2));
        painter.setPen(Qt::white);
        painter.drawText(this->rect(), Qt::AlignCenter,
                         QStringLiteral("Let go to dock over Twitch chat"));
    }
};

}  // namespace

#endif

TwitchChatDock::TwitchChatDock(Window &window)
    : QObject(&window)
    , window_(window)
{
#ifdef USEWINSDK
    QApplication::instance()->installNativeEventFilter(this);
    this->followTimer_.setInterval(FOLLOW_MS);
    QObject::connect(&this->followTimer_, &QTimer::timeout, this, [this] {
        this->followBrowser();
    });
    this->dragTimer_.setInterval(DRAG_CHECK_MS);
    QObject::connect(&this->dragTimer_, &QTimer::timeout, this, [this] {
        this->checkDragMoved();
        if (this->moved_ && getSettings()->dockOverTwitchChat)
        {
            this->onDragMoved();
        }
    });
#endif
}

TwitchChatDock::~TwitchChatDock()
{
#ifdef USEWINSDK
    if (auto *app = QApplication::instance())
    {
        app->removeNativeEventFilter(this);
    }
    delete this->preview_.data();
#endif
}

bool TwitchChatDock::nativeEventFilter(const QByteArray & /*eventType*/,
                                       void *message, qintptr * /*result*/)
{
#ifdef USEWINSDK
    auto *msg = static_cast<MSG *>(message);
    // winId() would create the window, and this also runs while it's being created
    this->windowHandle_ = quintptr(this->window_.internalWinId());
    if (this->windowHandle_ == 0)
    {
        return false;
    }
    if (reinterpret_cast<quintptr>(msg->hwnd) != this->windowHandle_)
    {
        return false;
    }

    switch (msg->message)
    {
        case WM_ENTERSIZEMOVE:
            this->dragging_ = true;
            this->moved_ = false;
            this->dragBrowser_ = 0;
            this->dragLocation_ = {};
            this->dragStartRect_ = windowRect(msg->hwnd);
            // Windows doesn't reliably say the window moved during a drag, so
            // it's checked on a timer, which keeps running through the drag
            this->dragTimer_.start();
            break;

        case WM_EXITSIZEMOVE:
            if (this->dragging_)
            {
                this->dragTimer_.stop();
                this->checkDragMoved();
                this->dragging_ = false;
                this->onDragFinished();
            }
            break;

        default:
            break;
    }
#else
    (void)message;
#endif
    return false;
}

#ifdef USEWINSDK

void TwitchChatDock::checkDragMoved()
{
    // A resize changes the size, and shouldn't count as being dragged anywhere
    const auto now = windowRect(toHwnd(this->windowHandle_));
    if (now.size() == this->dragStartRect_.size() &&
        now.topLeft() != this->dragStartRect_.topLeft())
    {
        this->moved_ = true;
    }
}

void TwitchChatDock::onDragMoved()
{
    HWND preview = this->preview_ ? HWND(this->preview_->winId()) : nullptr;
    HWND browser =
        twitchBrowserUnderCursor(toHwnd(this->windowHandle_), preview);

    if (browser == nullptr)
    {
        this->dragBrowser_ = 0;
        this->dragLocation_ = {};
        this->hidePreview();
        return;
    }

    const auto browserId = reinterpret_cast<quintptr>(browser);
    if (browserId != this->dragBrowser_)
    {
        this->dragBrowser_ = browserId;
        this->dragLocation_ = {};
        this->hidePreview();
        this->sinceDragSearch_.invalidate();
    }

    if (!this->sinceDragSearch_.isValid() ||
        this->sinceDragSearch_.elapsed() >= DRAG_SEARCH_MS)
    {
        this->sinceDragSearch_.start();
        this->findChat(browserId);
    }

    POINT cursor{};
    ::GetCursorPos(&cursor);
    if (this->dragLocation_.browser == browserId &&
        this->dragLocation_.chat.contains(cursor.x, cursor.y))
    {
        this->showPreview(this->dragLocation_.chat);
    }
    else
    {
        this->hidePreview();
    }
}

void TwitchChatDock::onDragFinished()
{
    const bool overChat = this->preview_ && this->preview_->isVisible();
    const auto location = this->dragLocation_;
    this->hidePreview();

    if (this->docked_)
    {
        if (overChat && location.browser == this->browser_)
        {
            // Let go back on the same chat, so it stays there
            this->placeOver(this->chatRect_);
        }
        else if (this->moved_)
        {
            const auto landed = windowRect(toHwnd(this->windowHandle_));
            this->undock();
            if (this->restoreGeometry_.isValid() && !this->restoreMaximized_)
            {
                ::SetWindowPos(toHwnd(this->windowHandle_), nullptr,
                               landed.left(), landed.top(), 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (overChat)
            {
                this->dock(location);
            }
        }
        return;
    }

    if (overChat && this->moved_)
    {
        this->dock(location);
    }
}

void TwitchChatDock::dock(const ChatLocation &location)
{
    if (this->docked_)
    {
        return;
    }
    auto hwnd = toHwnd(this->windowHandle_);

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    ::GetWindowPlacement(hwnd, &placement);
    this->restoreMaximized_ = placement.showCmd == SW_SHOWMAXIMIZED;
    this->restoreGeometry_ = toQRect(placement.rcNormalPosition);
    if (this->restoreMaximized_)
    {
        ::ShowWindow(hwnd, SW_RESTORE);
    }

    auto &notebook = this->window_.getNotebook();
    this->restoreShowTabs_ = notebook.getShowTabs();
    this->previousPage_ = notebook.getSelectedPage();
    // Hiding tabs normally explains itself in a popup, which isn't wanted here
    const bool inform = getSettings()->informOnTabVisibilityToggle.getValue();
    getSettings()->informOnTabVisibilityToggle.setValue(false);
    notebook.setShowTabs(false);
    getSettings()->informOnTabVisibilityToggle.setValue(inform);

    this->docked_ = true;
    this->hiddenByDock_ = false;
    this->usedExactLocation_ = location.exact;
    this->browser_ = location.browser;
    this->browserRect_ = windowRect(toHwnd(this->browser_));
    this->chatRect_ = location.chat;
    this->channel_.clear();

    this->placeOver(this->chatRect_);
    this->showChannelFromTitle();
    this->sinceSearch_.start();
    this->followTimer_.start();
    this->setTabsButtonVisible(true);
}

void TwitchChatDock::setTabsButtonVisible(bool visible)
{
    if (visible && !this->tabsButton_ && this->window_.hasCustomWindowFrame())
    {
        this->tabsButton_ = this->window_.addTitleBarLabel([this] {
            this->showTabsMenu();
        });
        this->tabsButton_->setText(QStringLiteral("Tabs ▾"));
        this->tabsButton_->setToolTip(
            QStringLiteral("Switch to your other tabs while docked"));
    }
    if (this->tabsButton_)
    {
        this->tabsButton_->setVisible(visible);
    }
}

void TwitchChatDock::showTabsMenu()
{
    auto &notebook = this->window_.getNotebook();
    QMenu menu;

    // Back to whatever the browser is showing
    auto *stream = menu.addAction(
        this->channel_.isEmpty()
            ? QStringLiteral("Back to stream chat")
            : QStringLiteral("Back to stream chat (%1)").arg(this->channel_));
    QObject::connect(stream, &QAction::triggered, this, [this] {
        this->channel_.clear();
        this->showChannelFromTitle();
    });
    menu.addSeparator();

    auto *selected = notebook.getSelectedPage();
    for (int i = 0; i < notebook.getPageCount(); i++)
    {
        auto *page = notebook.getPageAt(i);
        if (page == nullptr || page == this->addedPage_.data())
        {
            continue;
        }
        auto *tab = notebook.getTabFromPage(page);
        if (tab == nullptr)
        {
            continue;
        }
        auto title = tab->getTitle();
        if (title.isEmpty())
        {
            title = QStringLiteral("Tab %1").arg(i + 1);
        }
        auto *action = menu.addAction(
            tab->isLive() ? QStringLiteral("● ") + title : title);
        action->setCheckable(true);
        action->setChecked(page == selected);
        if (tab->isLive())
        {
            action->setToolTip(QStringLiteral("Live"));
        }
        QPointer<QWidget> target(page);
        QObject::connect(action, &QAction::triggered, this, [this, target] {
            if (target)
            {
                this->window_.getNotebook().select(target, true);
            }
        });
    }

    menu.addSeparator();
    auto *showBar = menu.addAction(QStringLiteral("Show tab bar while docked"));
    showBar->setCheckable(true);
    showBar->setChecked(notebook.getShowTabs());
    QObject::connect(showBar, &QAction::toggled, this, [this](bool show) {
        // Toggling tabs normally explains itself in a popup; not wanted here
        const bool inform =
            getSettings()->informOnTabVisibilityToggle.getValue();
        getSettings()->informOnTabVisibilityToggle.setValue(false);
        this->window_.getNotebook().setShowTabs(show);
        getSettings()->informOnTabVisibilityToggle.setValue(inform);
    });

    const auto *button = this->tabsButton_.data();
    const QPoint at = button != nullptr
                          ? button->mapToGlobal(button->rect().bottomLeft())
                          : QCursor::pos();
    menu.exec(at);
}

void TwitchChatDock::undock(bool immediately)
{
    if (!this->docked_)
    {
        return;
    }
    this->docked_ = false;
    this->followTimer_.stop();
    this->setTabsButtonVisible(false);
    auto hwnd = toHwnd(this->windowHandle_);

    if (this->hiddenByDock_)
    {
        this->hiddenByDock_ = false;
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    ::GetWindowPlacement(hwnd, &placement);
    placement.rcNormalPosition = {
        this->restoreGeometry_.left(), this->restoreGeometry_.top(),
        this->restoreGeometry_.left() + this->restoreGeometry_.width(),
        this->restoreGeometry_.top() + this->restoreGeometry_.height()};
    placement.showCmd =
        this->restoreMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
    ::SetWindowPlacement(hwnd, &placement);

    if (immediately)
    {
        this->restoreTabs();
    }
    else
    {
        // Switching tabs slides between pages at the size they had when it
        // started, so the window has to have its real size first
        QTimer::singleShot(0, this, [this] {
            this->restoreTabs();
        });
    }
}

void TwitchChatDock::restoreTabs()
{
    if (this->docked_)
    {
        return;
    }
    auto &notebook = this->window_.getNotebook();
    if (this->previousPage_ != nullptr)
    {
        notebook.select(this->previousPage_, false);
    }
    if (this->addedPage_)
    {
        notebook.removePage(this->addedPage_.data());
    }
    this->addedPage_.clear();
    this->addedSplit_.clear();
    this->previousPage_ = nullptr;
    notebook.setShowTabs(this->restoreShowTabs_);

    // Windows can finish giving the window back its real size after this, and
    // the tabs were last laid out for the narrow docked chat. Lay them out once
    // more when the size has settled so every tab lands in its row.
    QTimer::singleShot(120, this, [this] {
        if (!this->docked_)
        {
            this->window_.getNotebook().refresh();
        }
    });
}

void TwitchChatDock::followBrowser()
{
    if (!this->docked_ || this->dragging_)
    {
        return;
    }
    auto hwnd = toHwnd(this->windowHandle_);
    auto browser = toHwnd(this->browser_);

    if (!::IsWindow(browser))
    {
        this->undock();
        return;
    }
    // Maximizing it, or sending it to the tray, means it's wanted elsewhere
    if (::IsZoomed(hwnd))
    {
        this->undock();
        return;
    }
    if (::IsIconic(hwnd) ||
        (!this->hiddenByDock_ && !this->window_.isVisible()))
    {
        return;
    }

    if (::IsIconic(browser) || !::IsWindowVisible(browser) ||
        !isShowingTwitch(browser))
    {
        this->setHidden(true);
        return;
    }

    const auto rect = windowRect(browser);
    if (rect != this->browserRect_)
    {
        const bool sameSize = rect.size() == this->browserRect_.size();
        const auto shift = rect.topLeft() - this->browserRect_.topLeft();
        this->browserRect_ = rect;
        this->chatRect_.translate(shift);
        if (!this->hiddenByDock_)
        {
            this->placeOver(this->chatRect_);
        }
        if (!sameSize)
        {
            this->findChat(this->browser_);
            this->sinceSearch_.start();
        }
    }
    else if (this->sinceSearch_.elapsed() >= DOCKED_SEARCH_MS)
    {
        this->sinceSearch_.start();
        this->findChat(this->browser_);
    }

    if (!this->hiddenByDock_)
    {
        this->keepAboveBrowser();
    }
    this->showChannelFromTitle();
}

void TwitchChatDock::placeOver(const QRect &rect)
{
    auto hwnd = toHwnd(this->windowHandle_);
    if (windowRect(hwnd) == rect)
    {
        return;
    }
    ::SetWindowPos(hwnd, nullptr, rect.x(), rect.y(), rect.width(),
                   rect.height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void TwitchChatDock::keepAboveBrowser()
{
    auto hwnd = toHwnd(this->windowHandle_);
    auto browser = toHwnd(this->browser_);

    HWND above = ::GetWindow(browser, GW_HWNDPREV);
    if (above == hwnd)
    {
        return;
    }

    if (::GetForegroundWindow() == hwnd)
    {
        // Using the docked chat brings its browser up with it
        ::SetWindowPos(browser, hwnd, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    else
    {
        // Directly above the browser, so other apps still cover both
        ::SetWindowPos(hwnd, above != nullptr ? above : HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void TwitchChatDock::setHidden(bool hidden)
{
    if (this->hiddenByDock_ == hidden)
    {
        return;
    }
    this->hiddenByDock_ = hidden;
    ::ShowWindow(toHwnd(this->windowHandle_),
                 hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
}

void TwitchChatDock::showChannelFromTitle()
{
    // Browsers title it "(3) name - Twitch - Brave"
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(?:\(\d+\)\s*)?([A-Za-z0-9_]{1,25}) - Twitch\b)"));
    const auto match = pattern.match(windowTitle(toHwnd(this->browser_)));
    if (!match.hasMatch())
    {
        return;
    }
    const auto login = match.captured(1).toLower();
    if (login != this->channel_)
    {
        this->selectChannel(login);
    }
}

void TwitchChatDock::selectChannel(const QString &login)
{
    this->channel_ = login;
    auto &notebook = this->window_.getNotebook();

    // Use the tab they already have for this channel, if there is one
    for (int i = 0; i < notebook.getPageCount(); i++)
    {
        auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
        if (page == nullptr || page == this->addedPage_.data())
        {
            continue;
        }
        for (auto *split : page->getSplits())
        {
            if (split->getChannel()->getName().compare(
                    login, Qt::CaseInsensitive) == 0)
            {
                notebook.select(page, false);
                return;
            }
        }
    }

    auto channel = getApp()->getTwitch()->getOrAddChannel(login);
    if (!this->addedPage_)
    {
        this->addedPage_ = notebook.addPage(true);
        this->addedSplit_ = new Split(this->addedPage_.data());
        this->addedPage_->insertSplit(this->addedSplit_.data());
    }
    this->addedSplit_->setChannel(channel);
    notebook.select(this->addedPage_.data(), false);
}

void TwitchChatDock::findChat(quintptr browser)
{
    if (this->searching_)
    {
        return;
    }
    this->searching_ = true;

    QPointer<TwitchChatDock> self(this);
    (void)QtConcurrent::run([self, browser] {
        ChatLocation location;
        location.browser = browser;
        const auto initialized =
            ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool found =
            readChatRect(toHwnd(browser), location.chat, location.exact);
        if (SUCCEEDED(initialized))
        {
            ::CoUninitialize();
        }
        if (!found)
        {
            location.chat = {};
        }
        QMetaObject::invokeMethod(
            QApplication::instance(),
            [self, location] {
                if (self)
                {
                    self->onChatFound(location);
                }
            },
            Qt::QueuedConnection);
    });
}

void TwitchChatDock::onChatFound(const ChatLocation &location)
{
    this->searching_ = false;

    if (this->dragging_ && location.browser == this->dragBrowser_)
    {
        this->dragLocation_ = location;
        return;
    }
    if (!this->docked_ || this->dragging_ || location.browser != this->browser_)
    {
        return;
    }

    if (location.chat.isNull())
    {
        // Browsers can briefly report nothing while the page changes, so only
        // hide when the chat that used to be found by name is really gone
        return;
    }

    if (location.exact)
    {
        this->usedExactLocation_ = true;
    }
    else if (this->usedExactLocation_)
    {
        // A page without chat, like the home page, or chat collapsed
        this->setHidden(true);
        return;
    }

    this->chatRect_ = location.chat;
    this->setHidden(false);
    if (!this->hiddenByDock_)
    {
        this->placeOver(this->chatRect_);
    }
}

void TwitchChatDock::showPreview(const QRect &rect)
{
    if (!this->preview_)
    {
        this->preview_ = new DockPreview();
    }
    if (this->preview_->isVisible())
    {
        return;
    }
    this->preview_->winId();
    ::SetWindowPos(HWND(this->preview_->winId()), HWND_TOPMOST, rect.x(),
                   rect.y(), rect.width(), rect.height(),
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    this->preview_->show();
}

void TwitchChatDock::hidePreview()
{
    if (this->preview_ && this->preview_->isVisible())
    {
        this->preview_->hide();
    }
}

#else

void TwitchChatDock::undock(bool /*immediately*/)
{
}

#endif

}  // namespace chatterino
