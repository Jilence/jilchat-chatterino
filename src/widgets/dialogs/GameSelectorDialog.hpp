// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/ChannelManagement.hpp"
#include "widgets/DraggablePopup.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <vector>

class QEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QShowEvent;

namespace chatterino {

class Button;
class Split;
class SvgButton;
class TwitchChannel;

/// Shows the user's own saved games. Clicking one sends the channel's game
/// command, normally "!game <name>", to the channel named in the popup's header.
/// Each channel keeps its own command. Typing searches Twitch, and clicking a
/// result adds it to the saved list without sending anything.
///
/// This is only a chat message. The category changes only if a bot in the
/// channel acts on the command. Changing it directly through Twitch is done by
/// ChannelManagementDialog and needs broadcaster or editor access.
class GameSelectorDialog : public DraggablePopup
{
public:
    GameSelectorDialog(std::shared_ptr<TwitchChannel> channel, Split *split);

    static void showDialog(const std::shared_ptr<TwitchChannel> &channel,
                           Split *split);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void followCurrentTab();
    void onSearchTextChanged();
    void search();
    void rebuildList();
    void addSavedRow(const QString &name);
    void addNewGameRow(const QString &name, const QString &label);
    void setStatus(const QString &text);
    void activateItem(QListWidgetItem *item);
    void addGame(const QString &name);
    void removeGame(const QString &name);
    void sendGame(const QString &name);
    bool isSaved(const QString &name) const;
    void refreshStyle();

    std::shared_ptr<TwitchChannel> channel_;
    QPointer<Split> split_;

    Button *pinButton_{};
    SvgButton *closeButton_{};
    QLabel *titleLabel_{};
    QLabel *channelLabel_{};
    QLabel *commandLabel_{};
    QLineEdit *commandInput_{};
    QLineEdit *searchInput_{};
    QListWidget *list_{};
    QLabel *statusLabel_{};
    QLabel *hintLabel_{};

    QTimer searchDebounce_;
    QStringList savedGames_;
    std::vector<ChannelManagementCategory> searchResults_;
    /// Identifies the newest search, so that slower, older responses are ignored
    int searchGeneration_ = 0;
    /// Stops a double-click from sending the command twice
    QElapsedTimer sinceLastSend_;
    /// Whether the popup is pinned, to remember it for next time
    bool pinned_ = false;
    /// Set once the popup has its first size, so only later resizes are kept
    bool sized_ = false;
};

}  // namespace chatterino
