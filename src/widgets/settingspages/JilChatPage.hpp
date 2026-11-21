// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QVBoxLayout;

namespace chatterino {

class GeneralPageView;
class LeafyrinoPage;
class MoltorinoPage;
class TechnorinoPage;

/// What is particular to JilChat Chatterino: JilTV, JilChat's voice messages,
/// the desktop presence and the public logs, and the settings that came from
/// Leafyrino, Moltorino and Technorino
class JilChatPage : public SettingsPage
{
    Q_OBJECT

public:
    JilChatPage();

    bool filterElements(const QString &query) override;

    /// Reveals the bot badge setup, which is hidden otherwise, and scrolls
    /// to it.
    void showBotBadgeSettings();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void initLayout(GeneralPageView &layout);
    void rebuildDesktopPresenceAccounts(QVBoxLayout *layout);

    GeneralPageView *view_{};
    LeafyrinoPage *leafyrino_{};
    MoltorinoPage *moltorino_{};
    TechnorinoPage *technorino_{};
};

}  // namespace chatterino
