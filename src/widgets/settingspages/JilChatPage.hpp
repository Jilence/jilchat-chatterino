// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QVBoxLayout;

namespace chatterino {

class GeneralPageView;

/// What is particular to JilChat Chatterino: JilChat's voice messages, the
/// desktop presence and the public logs
class JilChatPage : public SettingsPage
{
    Q_OBJECT

public:
    JilChatPage();

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);
    void rebuildDesktopPresenceAccounts(QVBoxLayout *layout);

    GeneralPageView *view_{};
};

}  // namespace chatterino
