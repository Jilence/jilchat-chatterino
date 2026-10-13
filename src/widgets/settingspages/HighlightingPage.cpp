// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightingPage.hpp"

#include "Application.hpp"
#include "controllers/highlights/BadgeHighlightModel.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistModel.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightChannelScope.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightModel.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/UserHighlightModel.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/BadgePickerDialog.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/ColorItemDelegate.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace chatterino {

namespace {

QList<DisplayBadge> availableBadges = {
    {"Broadcaster", "broadcaster"},
    {"Admin", "admin"},
    {"Staff", "staff"},
    {"Moderator", "moderator"},
    {"Lead Moderator", "lead_moderator"},
    {"Verified", "partner"},
    {"VIP", "vip"},
    {"Founder", "founder"},
    {"Subscriber", "subscriber"},
    {"Predicted Blue", "predictions/blue-1,predictions/blue-2"},
    {"Predicted Pink", "predictions/pink-2,predictions/pink-1"},
};
}

namespace {

/// Shows what a marker looks like on a piece of text
class MatchAppearancePreview final : public QWidget
{
public:
    explicit MatchAppearancePreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setAppearance(QColor color, HighlightMatchStyle style)
    {
        this->color_ = color;
        this->style_ = style;
        this->update();
    }

    /// Room for the whole example. This is asked for when the dialog is laid
    /// out, with the font the widget has by then.
    QSize minimumSizeHint() const override
    {
        const auto metrics = this->fontMetrics();
        return {
            metrics.horizontalAdvance("Example message with matched text") + 32,
            std::max(42, metrics.height() + 16)};
    }

    QSize sizeHint() const override
    {
        return this->minimumSizeHint();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(this->rect(), this->palette().base());

        const QString prefix = "Example message with ";
        const QString matched = "matched text";
        const auto metrics = this->fontMetrics();
        const auto baseline =
            (this->height() + metrics.ascent() - metrics.descent()) / 2;
        const auto left = 10;
        painter.setFont(this->font());
        painter.setPen(this->palette().text().color());
        painter.drawText(left, baseline, prefix);

        const QRectF markerRect(left + metrics.horizontalAdvance(prefix) - 2,
                                (this->height() - metrics.height()) / 2.0,
                                metrics.horizontalAdvance(matched) + 4,
                                metrics.height());
        const FragmentHighlight highlight{
            .match = {.color = this->color_, .style = this->style_},
        };
        paintFragmentHighlightBackground(painter, markerRect, highlight);
        painter.setPen(this->palette().text().color());
        painter.drawText(QPointF(markerRect.left() + 2, baseline), matched);
        paintFragmentHighlightForeground(painter, markerRect, highlight);
    }

private:
    QColor color_{Qt::red};
    HighlightMatchStyle style_{HighlightMatchStyle::Outline};
};

/// Asks for the style and color of a marker. Taken from Moltorino.
class MatchAppearanceDialog final : public QDialog
{
public:
    MatchAppearanceDialog(QWidget *parent, QColor color,
                          HighlightMatchStyle style)
        : QDialog(parent)
        , color_(color)
    {
        this->setWindowTitle("Marker of the matched text");
        this->setAttribute(Qt::WA_DeleteOnClose);

        auto *layout = new QFormLayout(this);

        this->style_ = new QComboBox(this);
        for (const auto candidate :
             {HighlightMatchStyle::None, HighlightMatchStyle::Outline,
              HighlightMatchStyle::Fill, HighlightMatchStyle::OutlineAndFill,
              HighlightMatchStyle::Underline})
        {
            this->style_->addItem(highlightMatchStyleName(candidate),
                                  static_cast<int>(candidate));
        }
        const auto styleIndex = this->style_->findData(static_cast<int>(style));
        this->style_->setCurrentIndex(styleIndex >= 0 ? styleIndex : 1);
        layout->addRow("Marker style:", this->style_);

        this->colorButton_ = new QPushButton(this);
        layout->addRow("Marker color:", this->colorButton_);

        this->preview_ = new MatchAppearancePreview(this);
        layout->addRow("Preview:", this->preview_);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addRow(buttons);

        QObject::connect(this->style_, &QComboBox::currentIndexChanged, this,
                         [this] {
                             this->updateState();
                         });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateState();
                                     }
                                 });
                picker->show();
            });
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);

        this->updateState();
    }

    QColor color() const
    {
        return this->color_;
    }

    HighlightMatchStyle style() const
    {
        return static_cast<HighlightMatchStyle>(
            this->style_->currentData().toInt());
    }

private:
    void updateState()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));
        // without a marker there is nothing to color
        this->colorButton_->setEnabled(this->style() !=
                                       HighlightMatchStyle::None);
        this->preview_->setAppearance(this->color_, this->style());
    }

    QColor color_;
    QComboBox *style_{};
    QPushButton *colorButton_{};
    MatchAppearancePreview *preview_{};
};

/// Asks for the channels a highlight applies in. Taken from Moltorino.
class ChannelScopeDialog final : public QDialog
{
public:
    ChannelScopeDialog(QWidget *parent, QString ruleName,
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
        this->channelInput_->setToolTip(
            "Separate multiple channels with commas.");
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
        this->channels_->header()->setSectionResizeMode(1,
                                                        QHeaderView::Stretch);
        channelsLayout->addWidget(this->channels_, 1);

        auto *channelButtons = new QHBoxLayout();
        this->remove_ =
            new QPushButton("Remove selected", this->channelsGroup_);
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
        QObject::connect(this->channels_, &QTreeWidget::itemSelectionChanged,
                         this, [this] {
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

    HighlightChannelScope scope() const
    {
        std::vector<HighlightChannelTarget> targets;
        targets.reserve(this->channels_->topLevelItemCount());
        for (int index = 0; index < this->channels_->topLevelItemCount();
             ++index)
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

private:
    HighlightChannelScopeMode mode() const
    {
        return static_cast<HighlightChannelScopeMode>(
            this->mode_->currentData().toInt());
    }

    bool addTarget(const HighlightChannelTarget &target)
    {
        const auto encoded = encodeHighlightChannelTarget(target);
        const auto normalizedChannel =
            normalizeHighlightChannelName(target.channel);
        for (int index = 0; index < this->channels_->topLevelItemCount();
             ++index)
        {
            const auto existing = decodeHighlightChannelTarget(
                this->channels_->topLevelItem(index)
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
            {highlightChannelTargetPlatformName(target.platform),
             target.channel});
        item->setData(0, Qt::UserRole, encoded);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        this->channels_->addTopLevelItem(item);
        return true;
    }

    void addInputChannels()
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

    void updateState()
    {
        const bool scoped =
            this->mode() != HighlightChannelScopeMode::Everywhere;
        this->channelsGroup_->setEnabled(scoped);
        const auto count = this->channels_->topLevelItemCount();
        this->summary_->setText(QStringLiteral("%1 selected %2")
                                    .arg(count)
                                    .arg(count == 1
                                             ? QStringLiteral("channel")
                                             : QStringLiteral("channels")));
        this->remove_->setEnabled(scoped &&
                                  !this->channels_->selectedItems().isEmpty());
    }

    QComboBox *mode_{};
    QGroupBox *channelsGroup_{};
    QComboBox *targetPlatform_{};
    QLineEdit *channelInput_{};
    QTreeWidget *channels_{};
    QPushButton *remove_{};
    QLabel *summary_{};
};

/// Tells which highlights a message would trigger, and why. Taken from
/// Moltorino.
class HighlightTesterDialog final : public QDialog
{
public:
    explicit HighlightTesterDialog(QWidget *parent)
        : QDialog(parent)
    {
        this->setWindowTitle("Test highlight rules");
        this->setMinimumSize(650, 520);
        auto *layout = new QVBoxLayout(this);

        auto *form = new QFormLayout();
        this->sender_ = new QLineEdit("someone", this);
        this->channel_ = new QLineEdit(this);
        this->channel_->setPlaceholderText("Optional channel name");
        this->platform_ = new QComboBox(this);
        this->platform_->addItem(
            "Twitch", static_cast<int>(MessagePlatform::AnyOrTwitch));
        this->platform_->addItem("YouTube",
                                 static_cast<int>(MessagePlatform::YouTube));
        this->platform_->addItem("Kick",
                                 static_cast<int>(MessagePlatform::Kick));
        form->addRow("Sender:", this->sender_);
        form->addRow("Channel:", this->channel_);
        form->addRow("Platform:", this->platform_);
        layout->addLayout(form);

        this->message_ = new QPlainTextEdit(this);
        this->message_->setPlaceholderText("Type or paste a test chat message");
        layout->addWidget(this->message_, 1);
        layout->addWidget(new QLabel("Preview:", this));
        this->preview_ = new QTextEdit(this);
        this->preview_->setReadOnly(true);
        this->preview_->setMaximumHeight(90);
        layout->addWidget(this->preview_);
        layout->addWidget(new QLabel("Match details:", this));
        this->explanation_ = new QPlainTextEdit(this);
        this->explanation_->setReadOnly(true);
        layout->addWidget(this->explanation_, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         &QDialog::accept);

        const auto update = [this] {
            this->updateResult();
        };
        QObject::connect(this->message_, &QPlainTextEdit::textChanged, this,
                         update);
        QObject::connect(this->sender_, &QLineEdit::textChanged, this, update);
        QObject::connect(this->channel_, &QLineEdit::textChanged, this, update);
        QObject::connect(this->platform_, &QComboBox::currentIndexChanged, this,
                         update);
        this->updateResult();
        this->resize(this->minimumSize());
    }

private:
    void updateResult()
    {
        const auto text = this->message_->toPlainText();
        this->preview_->setPlainText(text);
        if (text.isEmpty())
        {
            this->explanation_->setPlainText(
                "Enter a message to see which rules match and why.");
            return;
        }
        if (getSettings()->isBlacklistedUser(this->sender_->text()))
        {
            this->explanation_->setPlainText(
                "No highlight: this sender is in Blacklisted Users.");
            return;
        }

        const auto platform = static_cast<MessagePlatform>(
            this->platform_->currentData().toInt());
        QStringList scopeRejections;
        const auto describeRejection = [this](const QString &kind,
                                              const HighlightPhrase &rule) {
            const auto channel = this->channel_->text().trimmed();
            const auto context =
                channel.isEmpty()
                    ? QStringLiteral("no channel was entered")
                    : QStringLiteral("%1 channel \"%2\"")
                          .arg(this->platform_->currentText(), channel);
            return QStringLiteral(
                       "%1 rule \"%2\" matched, but where it applies "
                       "does not include this test because %3. Applies "
                       "in: %4.")
                .arg(kind, rule.getPattern(), context,
                     highlightChannelScopeSummary(
                         rule.getChannelScope().mode(),
                         rule.getChannelScope().targets().size()));
        };
        const auto phrases = getSettings()->highlightedMessages.readOnly();
        for (const auto &rule : *phrases)
        {
            if (rule.isMatch(text) && !rule.getChannelScope().appliesTo(
                                          platform, this->channel_->text()))
            {
                scopeRejections.append(
                    describeRejection("Message highlight", rule));
            }
        }
        const auto users = getSettings()->highlightedUsers.readOnly();
        for (const auto &rule : *users)
        {
            if (rule.isMatch(this->sender_->text()) &&
                !rule.getChannelScope().appliesTo(platform,
                                                  this->channel_->text()))
            {
                scopeRejections.append(
                    describeRejection("User highlight", rule));
            }
        }
        const auto [highlighted, result] = getApp()->getHighlights()->check(
            MessageParseArgs{}, {}, this->sender_->text(), text, MessageFlags{},
            platform, this->channel_->text(), true);

        QStringList explanation;
        if (!highlighted)
        {
            explanation.append(
                scopeRejections.isEmpty()
                    ? QStringLiteral("No rules matched.")
                    : QStringLiteral("No highlight was applied."));
        }
        else if (result.matches.empty())
        {
            explanation.append(
                "The message is highlighted by a whole message rule or a "
                "zero width regular expression, so there is no text "
                "fragment to mark.");
        }

        auto *document = this->preview_->document();
        for (const auto &match : result.matches)
        {
            QTextCursor cursor(document);
            cursor.setPosition(static_cast<int>(match.start));
            cursor.setPosition(static_cast<int>(match.start + match.length),
                               QTextCursor::KeepAnchor);
            if (match.style != HighlightMatchStyle::None)
            {
                QTextCharFormat format;
                format.setBackground(match.color);
                cursor.mergeCharFormat(format);
            }

            const auto matchedText =
                text.mid(match.start, match.length).replace(u'\n', u' ');
            if (match.source == HighlightMatchSource::AutoMod)
            {
                auto reason = match.ruleName;
                reason.replace(u'\n', u' ');
                explanation.append(
                    QString("Twitch AutoMod rule \"%1\" matched \"%2\" at "
                            "characters %3 through %4.")
                        .arg(reason, matchedText)
                        .arg(match.start + 1)
                        .arg(match.start + match.length));
            }
            else
            {
                QString mode = "phrase";
                if (const auto found = std::ranges::find_if(
                        *phrases,
                        [&](const auto &phrase) {
                            return phrase.getPattern() == match.pattern;
                        });
                    found != phrases->end())
                {
                    mode = found->isRegex() ? "regular expression"
                                            : "literal phrase";
                    mode += found->isCaseSensitive() ? ", case sensitive"
                                                     : ", case insensitive";
                }
                explanation.append(
                    QString("Highlight %1 \"%2\" matched \"%3\" at "
                            "characters %4 through %5.")
                        .arg(mode, match.pattern, matchedText)
                        .arg(match.start + 1)
                        .arg(match.start + match.length));
            }
        }
        explanation.append(scopeRejections);
        this->explanation_->setPlainText(explanation.join(u'\n'));
    }

    QLineEdit *sender_{};
    QLineEdit *channel_{};
    QComboBox *platform_{};
    QPlainTextEdit *message_{};
    QTextEdit *preview_{};
    QPlainTextEdit *explanation_{};
};

}  // namespace

HighlightingPage::HighlightingPage()
{
    LayoutCreator<HighlightingPage> layoutCreator(this);

    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();
    {
        auto tabs = layout.emplace<QTabWidget>();
        {
            auto highlights = tabs.appendTab(new QVBoxLayout, "Messages");
            {
                highlights.emplace<QLabel>(
                    "Play notification sounds and highlight messages based on "
                    "certain patterns.\n"
                    "Message highlights are prioritized over badge highlights "
                    "and user highlights.");

                auto *view =
                    highlights
                        .emplace<EditableModelView>(
                            (new HighlightModel(nullptr))
                                ->initialized(
                                    &getSettings()->highlightedMessages))
                        .getElement();
                view->addRegexHelpLink();
                view->setTitles(
                    {"Pattern", "Show in\nMentions", "Flash\ntaskbar",
                     "Enable\nregex", "Case-\nsensitive", "Play\nsound",
                     "Custom\nsound", "Color", "Marker", "Applies in"});
                this->addChannelScopeButton(view);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->setItemDelegateForColumn(
                    HighlightModel::Column::Color, new ColorItemDelegate(view));

                QTimer::singleShot(1, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 400);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedMessages.append(HighlightPhrase{
                        "my phrase", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Messages);
                                 });

                highlights.append(this->createCheckBox(
                    "Mark the exact part of a message that triggered a "
                    "highlight",
                    getSettings()->highlightMatchedFragments));
                auto tools = highlights.emplace<QHBoxLayout>().withoutMargin();
                auto tester = tools.emplace<QPushButton>("Test highlights...");
                tools->addStretch(1);
                QObject::connect(
                    tester.getElement(), &QPushButton::clicked, this, [this] {
                        auto *dialog = new HighlightTesterDialog(this);
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->open();
                    });
            }

            auto pingUsers = tabs.appendTab(new QVBoxLayout, "Users");
            {
                pingUsers.emplace<QLabel>(
                    "Play notification sounds and highlight messages from "
                    "certain users.\n"
                    "User highlights are prioritized over badge highlights, "
                    "but under message highlights.");
                EditableModelView *view =
                    pingUsers
                        .emplace<EditableModelView>(
                            (new UserHighlightModel(nullptr))
                                ->initialized(&getSettings()->highlightedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::UseRegex);
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::CaseSensitive);

                view->setTitles(
                    {"Username", "Show in\nMentions", "Flash\ntaskbar",
                     "Enable\nregex", "Case-\nsensitive", "Play\nsound",
                     "Custom\nsound", "Color", "Marker", "Applies in"});
                this->addChannelScopeButton(view);
                // a user has no text to mark
                view->getTableView()->hideColumn(
                    HighlightModel::Column::MatchAppearance);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->setItemDelegateForColumn(
                    UserHighlightModel::Column::Color,
                    new ColorItemDelegate(view));

                QTimer::singleShot(1, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedUsers.append(HighlightPhrase{
                        "highlighted user", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Users);
                                 });
            }

            auto badgeHighlights = tabs.appendTab(new QVBoxLayout, "Badges");
            {
                badgeHighlights.emplace<QLabel>(
                    "Play notification sounds and highlight messages based on "
                    "user badges.\n"
                    "Badge highlights are prioritized under user and message "
                    "highlights.");
                auto *view = badgeHighlights
                                 .emplace<EditableModelView>(
                                     (new BadgeHighlightModel(nullptr))
                                         ->initialized(
                                             &getSettings()->highlightedBadges))
                                 .getElement();
                view->setTitles({"Name", "Show In\nMentions", "Flash\ntaskbar",
                                 "Play\nsound", "Custom\nsound", "Color"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->setItemDelegateForColumn(
                    BadgeHighlightModel::Column::Color,
                    new ColorItemDelegate(view));

                QTimer::singleShot(1, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                std::ignore = view->addButtonPressed.connect([this] {
                    auto d = std::make_shared<BadgePickerDialog>(
                        availableBadges, this);

                    d->setWindowTitle("Choose badge");
                    if (d->exec() == QDialog::Accepted)
                    {
                        auto s = d->getSelection();
                        if (!s)
                        {
                            return;
                        }
                        getSettings()->highlightedBadges.append(
                            HighlightBadge{s->badgeName(), s->displayName(),
                                           false, false, false, "",
                                           *ColorProvider::instance().color(
                                               ColorType::SelfHighlight)});
                    }
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Badges);
                                 });
            }

            auto disabledUsers =
                tabs.appendTab(new QVBoxLayout, "Blacklisted Users");
            {
                disabledUsers.emplace<QLabel>(
                    "Disable notification sounds and highlights from certain "
                    "users (e.g. bots).");
                EditableModelView *view =
                    disabledUsers
                        .emplace<EditableModelView>(
                            (new HighlightBlacklistModel(nullptr))
                                ->initialized(&getSettings()->blacklistedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->setTitles({"Username", "Enable\nregex"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);

                QTimer::singleShot(1, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->blacklistedUsers.append(
                        HighlightBlacklistUser{"blacklisted user", false});
                });
            }
        }

        auto customSound = layout.emplace<QHBoxLayout>().withoutMargin();
        {
            auto label = customSound.append(this->createLabel<QString>(
                [](const auto &value) {
                    if (value.isEmpty())
                    {
                        return QString("Default sound: Chatterino Ping");
                    }

                    auto url = QUrl::fromLocalFile(value);
                    return QString("Default sound: <a href=\"%1\"><span "
                                   "style=\"color: white\">%2</span></a>")
                        .arg(url.toString(QUrl::FullyEncoded),
                             shortenString(url.fileName(), 50));
                },
                getSettings()->pathHighlightSound));
            label->setToolTip(
                "This sound will play for all highlight phrases that have "
                "sound enabled and don't have a custom sound set.");
            label->setTextFormat(Qt::RichText);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                           Qt::LinksAccessibleByKeyboard);
            label->setOpenExternalLinks(true);
            customSound->setStretchFactor(label.getElement(), 1);

            auto clearSound = customSound.emplace<QPushButton>("Clear");
            auto selectFile = customSound.emplace<QPushButton>("Change...");

            QObject::connect(selectFile.getElement(), &QPushButton::clicked,
                             this, [this]() mutable {
                                 auto fileName = QFileDialog::getOpenFileName(
                                     this, tr("Open Sound"), "",
                                     tr("Audio Files (*.mp3 *.wav)"));

                                 getSettings()->pathHighlightSound = fileName;
                             });
            QObject::connect(clearSound.getElement(), &QPushButton::clicked,
                             this, [=]() mutable {
                                 getSettings()->pathHighlightSound = QString();
                             });

            getSettings()->pathHighlightSound.connect(
                [clearSound = clearSound.getElement()](const auto &value) {
                    if (value.isEmpty())
                    {
                        clearSound->hide();
                    }
                    else
                    {
                        clearSound->show();
                    }
                },
                this->managedConnections_);
        }

        layout.append(createCheckBox(
            "Play highlight sound even when Chatterino is focused",
            getSettings()->highlightAlwaysPlaySound));
        layout.append(createCheckBox(
            "Flash taskbar only stops highlighting when Chatterino is focused",
            getSettings()->longAlerts));
    }

    this->disabledUsersChangedTimer_.setSingleShot(true);
}

void HighlightingPage::openSoundDialog(const QModelIndex &clicked,
                                       EditableModelView *view, int soundColumn)
{
    auto fileUrl = QFileDialog::getOpenFileUrl(this, tr("Open Sound"), QUrl(),
                                               tr("Audio Files (*.mp3 *.wav)"));
    view->getModel()->setData(clicked, fileUrl, Qt::UserRole);
    view->getModel()->setData(clicked, fileUrl.fileName(), Qt::DisplayRole);
}

void HighlightingPage::openColorDialog(const QModelIndex &clicked,
                                       EditableModelView *view,
                                       HighlightTab tab)
{
    auto initial =
        view->getModel()->data(clicked, Qt::DecorationRole).value<QColor>();

    auto *dialog = new ColorPickerDialog(initial, this);

    QObject::connect(dialog, &ColorPickerDialog::colorConfirmed, this,
                     [=](auto selected) {
                         if (selected.isValid())
                         {
                             view->getModel()->setData(clicked, selected,
                                                       Qt::DecorationRole);
                         }
                     });
    dialog->show();
}

void HighlightingPage::openChannelScopeDialog(const QModelIndex &clicked,
                                              EditableModelView *view)
{
    if (!clicked.isValid() || !clicked.flags().testFlag(Qt::ItemIsEnabled))
    {
        return;
    }

    const auto scope = HighlightModel::channelScopeFromData(
        view->getModel()->data(clicked, HighlightModel::ChannelScopeRole));
    const auto ruleName =
        view->getModel()
            ->data(view->getModel()->index(clicked.row(),
                                           HighlightModel::Column::Pattern),
                   Qt::DisplayRole)
            .toString();
    const QPersistentModelIndex target(clicked);
    QPointer<ChannelScopeDialog> dialog =
        new ChannelScopeDialog(this, ruleName, scope);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted || !target.isValid())
    {
        return;
    }

    const auto updated = dialog->scope();
    view->getModel()->setData(target, HighlightModel::channelScopeData(updated),
                              HighlightModel::ChannelScopeRole);
    view->getModel()->setData(
        target,
        highlightChannelScopeSummary(updated.mode(), updated.targets().size()),
        Qt::DisplayRole);
    view->getModel()->setData(
        target,
        highlightChannelScopeDescription(updated.mode(), updated.targets()),
        Qt::ToolTipRole);
}

void HighlightingPage::addChannelScopeButton(EditableModelView *view)
{
    auto *button = new QPushButton("Edit where it applies", view);
    button->setMinimumWidth(std::max(125, button->sizeHint().width()));
    button->setToolTip(
        "Choose the channels where the selected custom highlight can run.");
    button->setEnabled(false);
    view->addCustomButton(button);

    const auto updateButton = [view, button](const QModelIndex &current) {
        if (!current.isValid())
        {
            button->setEnabled(false);
            return;
        }
        const auto scopeIndex = view->getModel()->index(
            current.row(), HighlightModel::Column::ChannelScope);
        button->setEnabled(scopeIndex.flags().testFlag(Qt::ItemIsEnabled));
    };
    QObject::connect(
        view->getTableView()->selectionModel(),
        &QItemSelectionModel::currentChanged, this,
        [updateButton](const QModelIndex &current, const QModelIndex &) {
            updateButton(current);
        });
    QObject::connect(button, &QPushButton::clicked, this, [this, view] {
        const auto current =
            view->getTableView()->selectionModel()->currentIndex();
        if (!current.isValid())
        {
            return;
        }
        this->openChannelScopeDialog(
            view->getModel()->index(current.row(),
                                    HighlightModel::Column::ChannelScope),
            view);
    });
}

void HighlightingPage::openMatchAppearanceDialog(const QModelIndex &clicked,
                                                 EditableModelView *view)
{
    auto *model = view->getModel();
    const auto color = model->data(clicked, Qt::DecorationRole).value<QColor>();
    const auto style = static_cast<HighlightMatchStyle>(
        model->data(clicked, HighlightModel::MatchStyleRole).toInt());

    auto *dialog = new MatchAppearanceDialog(this, color, style);
    QObject::connect(
        dialog, &QDialog::accepted, this, [model, clicked, dialog] {
            model->setData(clicked, dialog->color(), Qt::DecorationRole);
            model->setData(clicked, static_cast<int>(dialog->style()),
                           HighlightModel::MatchStyleRole);
            model->setData(clicked, highlightMatchStyleName(dialog->style()),
                           Qt::DisplayRole);
        });
    dialog->show();
}

void HighlightingPage::tableCellClicked(const QModelIndex &clicked,
                                        EditableModelView *view,
                                        HighlightTab tab)
{
    if (!clicked.flags().testFlag(Qt::ItemIsEnabled))
    {
        return;
    }

    switch (tab)
    {
        case HighlightTab::Messages:
        case HighlightTab::Users: {
            using Column = HighlightModel::Column;

            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
            else if (clicked.column() == Column::MatchAppearance)
            {
                this->openMatchAppearanceDialog(clicked, view);
            }
            else if (clicked.column() == Column::ChannelScope)
            {
                this->openChannelScopeDialog(clicked, view);
            }
        }
        break;

        case HighlightTab::Badges: {
            using Column = BadgeHighlightModel::Column;
            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
        }
        break;

        case HighlightTab::Blacklist:
            break;
    }
}

}  // namespace chatterino
