// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/BluzyrinoBadgesDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/twitch/TwitchAccount.hpp"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

    QPointer<BluzyrinoBadgesDialog> &activeDialog()
    {
        static QPointer<BluzyrinoBadgesDialog> instance;
        return instance;
    }

    QString currentUserId()
    {
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (!account || account->isAnon())
        {
            return {};
        }
        return account->getUserId();
    }

}  // namespace

BluzyrinoBadgesDialog::BluzyrinoBadgesDialog(QWidget *parent)
    : QDialog(parent)
{
    this->setWindowTitle("My Bluzyrino badges");
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->userId_ = currentUserId();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *intro = new QLabel("Choose one donor badge to display. Special badges "
                             "can be shown or hidden individually.");
    intro->setWordWrap(true);
    root->addWidget(intro);

    this->contentLayout_ = new QVBoxLayout();
    this->contentLayout_->setSpacing(6);
    root->addLayout(this->contentLayout_);

    root->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::close);
    root->addWidget(buttons);

    this->resize(360, 240);
    this->rebuild();
}

void BluzyrinoBadgesDialog::showDialog(QWidget *parent)
{
    auto &instance = activeDialog();
    if (instance)
    {
        instance->rebuild();
        instance->raise();
        instance->activateWindow();
        return;
    }

    auto *dialog = new BluzyrinoBadgesDialog(parent);
    instance = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void BluzyrinoBadgesDialog::rebuild()
{
    // Clear previous content.
    while (QLayoutItem *item = this->contentLayout_->takeAt(0))
    {
        if (auto *w = item->widget())
        {
            w->deleteLater();
        }
        delete item;
    }

    auto *badges = getApp()->getBluzyrinoBadges();
    const auto catalog = badges->catalog();
    const auto owned = badges->availableBadgeIds(this->userId_);

    if (this->userId_.isEmpty())
    {
        auto *msg = new QLabel("Log in with a Twitch account to manage your "
                               "Bluzyrino badges.");
        msg->setWordWrap(true);
        this->contentLayout_->addWidget(msg);
        return;
    }

    auto categoryOf = [&](const QString &id) -> QString {
        auto it = std::find_if(catalog.begin(), catalog.end(),
                               [&](const BluzyrinoBadge &b) {
                                   return b.id == id;
                               });
        return it == catalog.end() ? QString{} : it->category;
    };
    auto tooltipOf = [&](const QString &id) -> QString {
        auto it = std::find_if(catalog.begin(), catalog.end(),
                               [&](const BluzyrinoBadge &b) {
                                   return b.id == id;
                               });
        return (it == catalog.end() || !it->emote) ? id
                                                   : it->emote->tooltip.string;
    };

    // --- Donor badges ---
    QStringList donorIds;
    for (const auto &id : owned)
    {
        if (categoryOf(id) == "donor")
        {
            donorIds.push_back(id);
        }
    }

    if (!donorIds.isEmpty())
    {
        auto *header = new QLabel("<b>Donor badge</b>");
        this->contentLayout_->addWidget(header);

        auto *group = new QButtonGroup(this);
        const auto selected = badges->selectedDonor(this->userId_);
        for (const auto &id : donorIds)
        {
            auto *radio = new QRadioButton(tooltipOf(id));
            radio->setChecked(id == selected);
            this->contentLayout_->addWidget(radio);
            group->addButton(radio);

            QObject::connect(radio, &QRadioButton::clicked, this,
                             [id, radio](bool checked) {
                                 if (!checked)
                                 {
                                     return;
                                 }
                                 radio->setEnabled(false);
                                 getApp()->getBluzyrinoBadges()->setDonorSelection(
                                     id, [radio](bool) {
                                         if (radio)
                                         {
                                             radio->setEnabled(true);
                                         }
                                     });
                             });
        }
    }

    // --- Special badges ---
    QStringList specialIds;
    for (const auto &id : owned)
    {
        if (categoryOf(id) == "special")
        {
            specialIds.push_back(id);
        }
    }

    if (!specialIds.isEmpty())
    {
        auto *header = new QLabel("<b>Special badges</b>");
        this->contentLayout_->addWidget(header);
        for (const auto &id : specialIds)
        {
            // Per-special show/hide is not yet wired to the registry; list them
            // so the user can see what they own.
            auto *label = new QLabel("• " + tooltipOf(id));
            this->contentLayout_->addWidget(label);
        }
    }

    if (donorIds.isEmpty() && specialIds.isEmpty())
    {
        auto *msg = new QLabel(
            "No donor or special badges are assigned to this Twitch account.");
        msg->setWordWrap(true);
        this->contentLayout_->addWidget(msg);
    }
}

}  // namespace chatterino
