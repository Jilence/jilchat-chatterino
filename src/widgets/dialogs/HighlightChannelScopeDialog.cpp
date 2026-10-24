// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/HighlightChannelScopeDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

HighlightChannelScopeDialog::HighlightChannelScopeDialog(
    QWidget *parent, const QString &ruleName,
    const HighlightChannelScope &existing)
    : QDialog(parent)
{
    this->setWindowTitle("Where this highlight applies");
    this->setMinimumSize(620, 440);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    auto *rule = new QLabel(QStringLiteral("Rule: %1").arg(ruleName), this);
    rule->setWordWrap(true);
    rule->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(rule);

    auto *modeForm = new QFormLayout();
    this->mode_ = new QComboBox(this);
    for (const auto mode : {HighlightChannelScopeMode::Everywhere,
                            HighlightChannelScopeMode::OnlySelected,
                            HighlightChannelScopeMode::ExcludeSelected})
    {
        this->mode_->addItem(highlightChannelScopeModeName(mode),
                             static_cast<int>(mode));
    }
    const auto modeIndex =
        this->mode_->findData(static_cast<int>(existing.mode()));
    this->mode_->setCurrentIndex(std::max(0, modeIndex));
    modeForm->addRow("Applies in:", this->mode_);
    layout->addLayout(modeForm);

    this->channelsGroup_ = new QGroupBox("Channels", this);
    auto *channelsLayout = new QVBoxLayout(this->channelsGroup_);

    auto *entryLayout = new QHBoxLayout();
    this->targetPlatform_ = new QComboBox(this->channelsGroup_);
    for (const auto platform : {HighlightChannelTargetPlatform::Any,
                                HighlightChannelTargetPlatform::Twitch,
                                HighlightChannelTargetPlatform::YouTube,
                                HighlightChannelTargetPlatform::Kick})
    {
        this->targetPlatform_->addItem(
            highlightChannelTargetPlatformName(platform),
            static_cast<int>(platform));
    }
    this->targetPlatform_->setMinimumWidth(125);
    this->targetPlatform_->setToolTip(
        "Use Any platform to match the channel on every service.");
    this->channelInput_ = new QLineEdit(this->channelsGroup_);
    this->channelInput_->setPlaceholderText(
        "Channel name, handle, ID, or link");
    this->channelInput_->setToolTip("Separate multiple channels with commas.");
    auto *add = new QPushButton("Add channels", this->channelsGroup_);
    add->setMinimumWidth(std::max(105, add->sizeHint().width()));
    entryLayout->addWidget(this->targetPlatform_);
    entryLayout->addWidget(this->channelInput_, 1);
    entryLayout->addWidget(add);
    channelsLayout->addLayout(entryLayout);

    this->channels_ = new QTreeWidget(this->channelsGroup_);
    this->channels_->setHeaderLabels({"Platform", "Channel"});
    this->channels_->setRootIsDecorated(false);
    this->channels_->setAlternatingRowColors(true);
    this->channels_->setUniformRowHeights(true);
    this->channels_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->channels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->channels_->header()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    this->channels_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    channelsLayout->addWidget(this->channels_, 1);

    auto *channelButtons = new QHBoxLayout();
    this->remove_ = new QPushButton("Remove selected", this->channelsGroup_);
    this->remove_->setEnabled(false);
    this->summary_ = new QLabel(this->channelsGroup_);
    channelButtons->addWidget(this->remove_);
    channelButtons->addStretch(1);
    channelButtons->addWidget(this->summary_);
    channelsLayout->addLayout(channelButtons);
    layout->addWidget(this->channelsGroup_, 1);

    for (const auto &target : existing.targets())
    {
        this->addTarget(target);
    }

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    QObject::connect(add, &QPushButton::clicked, this, [this] {
        this->addInputChannels();
    });
    QObject::connect(this->channelInput_, &QLineEdit::returnPressed, this,
                     [this] {
                         this->addInputChannels();
                     });
    QObject::connect(this->remove_, &QPushButton::clicked, this, [this] {
        auto selected = this->channels_->selectedItems();
        for (auto *item : selected)
        {
            delete item;
        }
        this->updateState();
    });
    QObject::connect(this->channels_, &QTreeWidget::itemSelectionChanged, this,
                     [this] {
                         this->remove_->setEnabled(
                             !this->channels_->selectedItems().isEmpty());
                     });
    QObject::connect(this->mode_, &QComboBox::currentIndexChanged, this,
                     [this] {
                         this->updateState();
                     });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (this->mode() != HighlightChannelScopeMode::Everywhere &&
            this->channels_->topLevelItemCount() == 0)
        {
            QMessageBox::warning(this, "Choose at least one channel",
                                 "Add a channel, or choose All channels.");
            return;
        }
        this->accept();
    });
    this->updateState();
}

HighlightChannelScope HighlightChannelScopeDialog::scope() const
{
    std::vector<HighlightChannelTarget> targets;
    targets.reserve(this->channels_->topLevelItemCount());
    for (int index = 0; index < this->channels_->topLevelItemCount(); ++index)
    {
        const auto encoded = this->channels_->topLevelItem(index)
                                 ->data(0, Qt::UserRole)
                                 .toString();
        if (auto target = decodeHighlightChannelTarget(encoded))
        {
            targets.emplace_back(std::move(*target));
        }
    }
    return {this->mode(), std::move(targets)};
}

HighlightChannelScopeMode HighlightChannelScopeDialog::mode() const
{
    return static_cast<HighlightChannelScopeMode>(
        this->mode_->currentData().toInt());
}

bool HighlightChannelScopeDialog::addTarget(
    const HighlightChannelTarget &target)
{
    const auto encoded = encodeHighlightChannelTarget(target);
    const auto normalizedChannel =
        normalizeHighlightChannelName(target.channel);
    for (int index = 0; index < this->channels_->topLevelItemCount(); ++index)
    {
        const auto existing =
            decodeHighlightChannelTarget(this->channels_->topLevelItem(index)
                                             ->data(0, Qt::UserRole)
                                             .toString());
        if (existing && existing->platform == target.platform &&
            normalizeHighlightChannelName(existing->channel) ==
                normalizedChannel)
        {
            return false;
        }
    }

    auto *item = new QTreeWidgetItem(
        {highlightChannelTargetPlatformName(target.platform), target.channel});
    item->setData(0, Qt::UserRole, encoded);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    this->channels_->addTopLevelItem(item);
    return true;
}

void HighlightChannelScopeDialog::addInputChannels()
{
    const auto platform = static_cast<HighlightChannelTargetPlatform>(
        this->targetPlatform_->currentData().toInt());
    bool added = false;
    for (auto channel : this->channelInput_->text().split(
             QRegularExpression(QStringLiteral("[,\\r\\n]")),
             Qt::SkipEmptyParts))
    {
        if (auto target =
                makeHighlightChannelTarget(platform, std::move(channel)))
        {
            added = this->addTarget(*target) || added;
        }
    }
    if (added)
    {
        this->channelInput_->clear();
    }
    this->channelInput_->setFocus();
    this->updateState();
}

void HighlightChannelScopeDialog::updateState()
{
    const bool scoped = this->mode() != HighlightChannelScopeMode::Everywhere;
    this->channelsGroup_->setEnabled(scoped);
    const auto count = this->channels_->topLevelItemCount();
    this->summary_->setText(QStringLiteral("%1 selected %2")
                                .arg(count)
                                .arg(count == 1 ? QStringLiteral("channel")
                                                : QStringLiteral("channels")));
    this->remove_->setEnabled(scoped &&
                              !this->channels_->selectedItems().isEmpty());
}

}  // namespace chatterino
