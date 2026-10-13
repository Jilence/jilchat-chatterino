// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightingPage.hpp"

#include "Application.hpp"
#include "controllers/highlights/BadgeHighlightModel.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistModel.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightModel.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/UserHighlightModel.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "providers/colors/ColorProvider.hpp"
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
#include <QHeaderView>
#include <QPainter>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>

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
                view->setTitles({"Pattern", "Show in\nMentions",
                                 "Flash\ntaskbar", "Enable\nregex",
                                 "Case-\nsensitive", "Play\nsound",
                                 "Custom\nsound", "Color", "Marker"});
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

                view->setTitles({"Username", "Show in\nMentions",
                                 "Flash\ntaskbar", "Enable\nregex",
                                 "Case-\nsensitive", "Play\nsound",
                                 "Custom\nsound", "Color", "Marker"});
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
            "Mark the text that matched a phrase (the Marker column)",
            getSettings()->highlightMatchedFragments));
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

namespace {

/// Shows what a marker looks like on a piece of text
class MatchAppearancePreview final : public QWidget
{
public:
    explicit MatchAppearancePreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumHeight(42);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setAppearance(QColor color, HighlightMatchStyle style)
    {
        this->color_ = color;
        this->style_ = style;
        this->update();
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

}  // namespace

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
