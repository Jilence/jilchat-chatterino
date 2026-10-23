// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/JilChatPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/notifications/DesktopPresenceController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/SvgWidget.hpp"
#include "widgets/settingspages/CustomWidgets.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>

namespace chatterino {

JilChatPage::JilChatPage()
{
    auto *outer = new QVBoxLayout;
    auto *inner = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;

    inner->addWidget(view);
    auto *frame = new QFrame;
    frame->setLayout(inner);
    outer->addWidget(frame);
    this->setLayout(outer);

    this->initLayout(*view);
}

bool JilChatPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }

    return false;
}

void JilChatPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    layout.addTitle("Voice messages");
    SettingWidget::intInput("Default voice message volume",
                            s.jilChatVoiceVolume,
                            {
                                .min = 0,
                                .max = 100,
                                .singleStep = 5,
                                .suffix = QStringLiteral("%"),
                            })
        ->addKeywords({"jilchat", "voice", "volume", "sound"})
        ->setTooltip("Controls the volume for JilChat voice messages.")
        ->addTo(layout);

    layout.addTitle("JilChat Desktop Presence");
    layout.addDescription(
        "Pause mobile notifications while JilChat Desktop is running.");
    auto *presence = new QWidget;
    auto *presenceLayout = new QVBoxLayout(presence);
    presenceLayout->setContentsMargins(0, 0, 0, 0);
    layout.addWidget(presence, {"jilchat", "desktop", "presence", "mobile",
                                "notifications", "pause"});
    this->rebuildDesktopPresenceAccounts(presenceLayout);
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->twitch.userListUpdated,
        [this, presenceLayout] {
            QTimer::singleShot(0, this, [this, presenceLayout] {
                this->rebuildDesktopPresenceAccounts(presenceLayout);
            });
        });
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->desktopPresence().changed,
        [this, presenceLayout] {
            QTimer::singleShot(0, this, [this, presenceLayout] {
                this->rebuildDesktopPresenceAccounts(presenceLayout);
            });
        });

    layout.addTitle("Public logs");
    layout.addDescription("Older messages from logs.zonian.dev, the logs "
                          "behind lurkology.com.");
    SettingWidget::checkbox("Use public logs",
                            s.loadOlderMessagesFromPublicLogs)
        ->setTooltip("Load older messages from logs.zonian.dev. This sends "
                     "channel names and usernames to that service.")
        ->addKeywords({"logs", "zonian", "lurkology", "supa", "history",
                       "older", "public"})
        ->addTo(layout);
    SettingWidget::intInput("Messages per load in chat", s.publicLogsPageSize,
                            {
                                .min = 10,
                                .max = 100,
                                .singleStep = 10,
                            })
        ->conditionallyEnabledBy(s.loadOlderMessagesFromPublicLogs)
        ->setTooltip("How many older messages are loaded from the public "
                     "logs each time you scroll to the top of a chat.")
        ->addKeywords({"logs", "zonian", "scroll", "page", "count", "messages",
                       "chat", "history", "older", "public"})
        ->addTo(layout);
    SettingWidget::checkbox("Load older messages in the usercard",
                            s.loadOlderUsercardMessages)
        ->setTooltip("Load older messages of a user on the usercard, beyond "
                     "what is in the chat. When disabled, the usercard only "
                     "shows messages from the chat.")
        ->addKeywords({"usercard", "messages", "history", "logs"})
        ->addTo(layout);
    SettingWidget::intInput("Messages per load in the usercard",
                            s.usercardOlderMessagesPageSize,
                            {
                                .min = 10,
                                .max = 100,
                                .singleStep = 10,
                            })
        ->conditionallyEnabledBy(s.loadOlderUsercardMessages)
        ->setTooltip("How many older messages the usercard loads from the "
                     "public logs at a time. The Twitch moderator history "
                     "loads in pages set by Twitch.")
        ->addKeywords({"usercard", "messages", "logs", "page", "count"})
        ->addTo(layout);

    // the rest of the page stays empty instead of spreading the sections
    layout.addStretch();
}

void JilChatPage::rebuildDesktopPresenceAccounts(QVBoxLayout *layout)
{
    while (auto *item = layout->takeAt(0))
    {
        if (auto *row = item->layout())
        {
            while (auto *child = row->takeAt(0))
            {
                delete child->widget();
                delete child;
            }
        }
        delete item->widget();
        delete item;
    }

    auto &controller = getApp()->getAccounts()->desktopPresence();
    const auto accounts = getApp()->getAccounts()->twitch.accounts.readOnly();
    if (accounts->empty())
    {
        layout->addWidget(new QLabel("No Twitch account is logged in."));
    }
    for (const auto &account : *accounts)
    {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);

        auto *checkBox = new SCheckBox(account->getUserName());
        checkBox->setChecked(controller.isEnabled(account->getUserId()));
        QObject::connect(checkBox, &QCheckBox::toggled, this,
                         [account, &controller](bool enabled) {
                             controller.setEnabled(account, enabled);
                         });
        row->addWidget(checkBox);

        // the same info icon as the other settings
        const auto hint =
            QStringLiteral("When the program is closed or crashes, "
                           "notifications resume automatically within 90 "
                           "seconds.");
        checkBox->setToolTip(hint);
        auto *icon = new SvgWidget;
        icon->load(QStringLiteral(":/settings/hint.svg"));
        icon->setToolTip(hint);
        const auto size = checkBox->sizeHint().height();
        const auto viewBox = icon->renderer()->viewBox();
        icon->setFixedSize(
            static_cast<int>(static_cast<double>(size) /
                             static_cast<double>(viewBox.height()) *
                             static_cast<double>(viewBox.width())),
            size);
        row->addWidget(icon);
        row->addStretch(1);
        layout->addLayout(row);

        const auto status = controller.statusText(account->getUserId());
        if (!status.isEmpty())
        {
            auto *statusLabel = new QLabel(status);
            statusLabel->setStyleSheet("color: palette(mid);");
            statusLabel->setContentsMargins(24, 0, 0, 0);
            layout->addWidget(statusLabel);
        }
    }
}

}  // namespace chatterino
