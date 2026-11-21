// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/GameSelectorDialog.hpp"

#include "Application.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/RapidJsonSerializeQSize.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/PopupControlMetrics.hpp"
#include "widgets/helper/InvisibleSizeGrip.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/QuickGameBar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(300, 360);
constexpr int SEARCH_DEBOUNCE_MS = 250;
constexpr int MIN_SEARCH_LENGTH = 2;
constexpr int MAX_NAME_LENGTH = 100;
constexpr int MAX_COMMAND_LENGTH = 50;
constexpr qint64 RESEND_GUARD_MS = 1000;

/// Where each list row keeps the game's name
constexpr int GAME_NAME_ROLE = Qt::UserRole;
/// True for rows that add a new game rather than send a saved one
constexpr int ADDS_GAME_ROLE = Qt::UserRole + 1;

QString labelFor(const ChannelManagementCategory &category)
{
    return category.displayName.isEmpty() ? category.name
                                          : category.displayName;
}

Split *currentSplit()
{
    auto *window = getApp()->getWindows()->getLastSelectedWindow();
    if (window == nullptr)
    {
        return nullptr;
    }
    auto *page = window->getNotebook().getSelectedPage();
    return page != nullptr ? page->getSelectedSplit() : nullptr;
}

}  // namespace

GameSelectorDialog::GameSelectorDialog(std::shared_ptr<TwitchChannel> channel,
                                       Split *split)
    : DraggablePopup(true, split != nullptr ? split->window() : nullptr)
    , channel_(std::move(channel))
    , split_(split)
    , searchDebounce_(this)
    , savedGames_(getSettings()->gameSelectorGames.getValue())
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("GameSelectorDialog");
    this->setWindowTitle("Change game");
    // The size it was last dragged to, else the default
    const auto saved = getSettings()->gameSelectorPopupSize.getValue();
    const auto size = saved.width() > 0 && saved.height() > 0
                          ? saved.boundedTo(QSize(3000, 3000))
                          : DEFAULT_DIALOG_SIZE;
    this->resize(size * this->scale());

    auto *container = this->getLayoutContainer();
    container->setObjectName("GameSelectorRoot");
    auto *layout = new QVBoxLayout(container);

    auto *header = new QHBoxLayout();
    auto *headerText = new QVBoxLayout();
    headerText->setContentsMargins(0, 0, 0, 0);
    headerText->setSpacing(0);
    this->titleLabel_ = new QLabel("Change game", container);
    this->titleLabel_->setObjectName("GameSelectorTitle");
    this->channelLabel_ =
        new QLabel("Sends to #" + this->channel_->getName(), container);
    this->channelLabel_->setObjectName("GameSelectorMuted");
    this->channelLabel_->setTextFormat(Qt::PlainText);
    headerText->addWidget(this->titleLabel_);
    headerText->addWidget(this->channelLabel_);
    header->addLayout(headerText, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip(
        "Pin: keep this open when you click elsewhere. It follows the tab you "
        "are using.");
    if (getSettings()->gameSelectorPinned)
    {
        this->pinned_ = this->ensurePinned();
    }
    QObject::connect(this->pinButton_, &Button::leftClicked, this, [this] {
        // The pin button flips the pin itself
        this->pinned_ = !this->pinned_;
        getSettings()->gameSelectorPinned = this->pinned_;
        std::ignore = getSettings()->requestSave();
    });
    header->addWidget(this->pinButton_, 0, Qt::AlignTop);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    header->addWidget(this->closeButton_, 0, Qt::AlignTop);
    layout->addLayout(header);

    auto *commandRow = new QHBoxLayout();
    commandRow->setContentsMargins(0, 0, 0, 0);
    this->commandLabel_ = new QLabel("Command", container);
    this->commandLabel_->setObjectName("GameSelectorMuted");
    commandRow->addWidget(this->commandLabel_);
    this->commandInput_ = new QLineEdit(container);
    this->commandInput_->setObjectName("GameSelectorSearch");
    this->commandInput_->setPlaceholderText("!game");
    this->commandInput_->setAccessibleName("Game command for this channel");
    this->commandInput_->setToolTip(
        "What this channel's bot uses to change game. It's saved for this "
        "channel only, and other channels keep their own.");
    this->commandInput_->setMaxLength(MAX_COMMAND_LENGTH);
    this->commandInput_->setText(
        quickgames::gameCommand(this->channel_->getName()));
    // Saved as you type, so a game clicked straight after uses the new command
    QObject::connect(this->commandInput_, &QLineEdit::textEdited, this,
                     [this](const QString &text) {
                         quickgames::setGameCommand(this->channel_->getName(),
                                                    text);
                         this->rebuildList();
                     });
    commandRow->addWidget(this->commandInput_, 1);
    layout->addLayout(commandRow);

    this->searchInput_ = new QLineEdit(container);
    this->searchInput_->setObjectName("GameSelectorSearch");
    this->searchInput_->setPlaceholderText("Type a game to add it");
    this->searchInput_->setAccessibleName("Search for a game to add");
    this->searchInput_->setClearButtonEnabled(true);
    this->searchInput_->setMaxLength(MAX_NAME_LENGTH);
    this->searchInput_->installEventFilter(this);
    layout->addWidget(this->searchInput_);

    this->list_ = new QListWidget(container);
    this->list_->setObjectName("GameSelectorList");
    this->list_->setAccessibleName("Games");
    this->list_->setMouseTracking(true);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setTextElideMode(Qt::ElideRight);
    this->list_->installEventFilter(this);
    layout->addWidget(this->list_, 1);

    this->statusLabel_ = new QLabel(container);
    this->statusLabel_->setObjectName("GameSelectorMuted");
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->setTextFormat(Qt::PlainText);
    this->statusLabel_->hide();
    layout->addWidget(this->statusLabel_);

    // Be upfront about what picking a game really does
    this->hintLabel_ = new QLabel(
        "Click a saved game to send the command to chat. A bot in this channel "
        "must support that command for the category to change.",
        container);
    this->hintLabel_->setObjectName("GameSelectorMuted");
    this->hintLabel_->setWordWrap(true);
    layout->addWidget(this->hintLabel_);

    // Drag the bottom right corner to resize it
    layout->addWidget(new InvisibleSizeGrip(this), 0,
                      Qt::AlignRight | Qt::AlignBottom);

    this->searchDebounce_.setSingleShot(true);
    this->searchDebounce_.setInterval(SEARCH_DEBOUNCE_MS);
    QObject::connect(&this->searchDebounce_, &QTimer::timeout, this,
                     &GameSelectorDialog::search);
    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this,
                     &GameSelectorDialog::onSearchTextChanged);

    // Enter is handled in eventFilter, so that this is the one way a mouse
    // click can act on a row.
    QObject::connect(this->list_, &QListWidget::itemClicked, this,
                     &GameSelectorDialog::activateItem);

    // When pinned, the popup stays open while you move between channels
    auto *follow = new QTimer(this);
    follow->setInterval(std::chrono::milliseconds(400));
    QObject::connect(follow, &QTimer::timeout, this, [this] {
        this->followCurrentTab();
    });
    follow->start();

    this->refreshStyle();
    this->onSearchTextChanged();
}

void GameSelectorDialog::followCurrentTab()
{
    auto *split = currentSplit();
    if (split == nullptr)
    {
        return;
    }

    auto channel =
        std::dynamic_pointer_cast<TwitchChannel>(split->getSelectedChannel());
    if (channel == nullptr)
    {
        // Tabs that aren't Twitch channels keep the last channel shown
        return;
    }

    this->split_ = split;
    if (channel != this->channel_)
    {
        this->channel_ = std::move(channel);
        this->channelLabel_->setText("Sends to #" + this->channel_->getName());
        this->commandInput_->setText(
            quickgames::gameCommand(this->channel_->getName()));
        this->rebuildList();
        this->setStatus({});
    }
}

void GameSelectorDialog::showDialog(
    const std::shared_ptr<TwitchChannel> &channel, Split *split)
{
    auto *dialog = new GameSelectorDialog(channel, split);

    QPoint center = QCursor::pos();
    if (split != nullptr && split->window() != nullptr)
    {
        center = split->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
}

void GameSelectorDialog::themeChangedEvent()
{
    if (this->list_ == nullptr)
    {
        return;
    }
    this->refreshStyle();
    this->rebuildList();
}

void GameSelectorDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    // Sizing the popup in the constructor can get here before the UI exists
    if (this->list_ == nullptr)
    {
        return;
    }
    this->refreshStyle();
    this->rebuildList();
}

void GameSelectorDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->searchInput_->setFocus();
    // Sizes from here on are the user's
    QTimer::singleShot(0, this, [this] {
        this->sized_ = true;
    });
}

void GameSelectorDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    if (!this->sized_ || !this->isVisible())
    {
        return;
    }
    const auto scale = std::max(0.1F, this->scale());
    getSettings()->gameSelectorPopupSize = QSize(
        int(float(this->width()) / scale), int(float(this->height()) / scale));
}

void GameSelectorDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
    {
        this->close();
        return;
    }
    DraggablePopup::keyPressEvent(event);
}

bool GameSelectorDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        const bool isEnter = key == Qt::Key_Return || key == Qt::Key_Enter;

        // Enter only moves to the list, so that a half-typed search can't act
        // on whatever happens to be the first row.
        if (watched == this->searchInput_ && (key == Qt::Key_Down || isEnter))
        {
            if (this->list_->count() > 0)
            {
                this->list_->setFocus();
                this->list_->setCurrentRow(0);
            }
            return true;
        }

        if (watched == this->list_)
        {
            if (key == Qt::Key_Up && this->list_->currentRow() <= 0)
            {
                this->list_->setCurrentRow(-1);
                this->searchInput_->setFocus();
                return true;
            }
            if (isEnter)
            {
                this->activateItem(this->list_->currentItem());
                return true;
            }
            if (key == Qt::Key_Delete &&
                this->list_->currentItem() != nullptr &&
                !this->list_->currentItem()->data(ADDS_GAME_ROLE).toBool())
            {
                this->removeGame(this->list_->currentItem()
                                     ->data(GAME_NAME_ROLE)
                                     .toString());
                return true;
            }
        }
    }

    return DraggablePopup::eventFilter(watched, event);
}

void GameSelectorDialog::onSearchTextChanged()
{
    // Any search still running is now out of date
    ++this->searchGeneration_;
    this->searchDebounce_.stop();
    this->searchResults_.clear();
    this->rebuildList();

    const auto query = this->searchInput_->text().simplified();
    if (query.size() >= MIN_SEARCH_LENGTH)
    {
        this->setStatus("Searching Twitch...");
        this->searchDebounce_.start();
    }
    else if (query.isEmpty() && this->savedGames_.isEmpty())
    {
        this->setStatus(
            "No games yet. Type a game above to add your first one.");
    }
    else
    {
        this->setStatus({});
    }
}

void GameSelectorDialog::search()
{
    const auto query = this->searchInput_->text().simplified();
    const auto generation = this->searchGeneration_;

    QPointer<GameSelectorDialog> self(this);
    ChannelManagement::searchCategories(
        query,
        [self, query,
         generation](std::vector<ChannelManagementCategory> categories) {
            if (!self || generation != self->searchGeneration_)
            {
                return;
            }
            ChannelManagement::rankCategoryResults(categories, query);
            self->searchResults_ = std::move(categories);
            self->rebuildList();
            self->setStatus(self->searchResults_.empty()
                                ? "No Twitch categories found. You can still "
                                  "add it as typed."
                                : QString());
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->searchGeneration_)
            {
                return;
            }
            self->setStatus(error + " You can still add it as typed.");
        });
}

void GameSelectorDialog::rebuildList()
{
    this->list_->clear();

    const auto query = this->searchInput_->text().simplified();

    for (const auto &name : std::as_const(this->savedGames_))
    {
        if (name.contains(query, Qt::CaseInsensitive))
        {
            this->addSavedRow(name);
        }
    }

    if (query.isEmpty())
    {
        return;
    }

    for (const auto &category : this->searchResults_)
    {
        const auto name = labelFor(category);
        if (!this->isSaved(name))
        {
            this->addNewGameRow(name, "+  " + name);
        }
    }

    // Bots and channels may use names Twitch doesn't, so typed text is allowed
    const bool inResults = std::ranges::any_of(
        this->searchResults_, [&query](const auto &category) {
            return labelFor(category).compare(query, Qt::CaseInsensitive) == 0;
        });
    if (!inResults && !this->isSaved(query))
    {
        this->addNewGameRow(query, "+  Add \"" + query + "\"");
    }
}

void GameSelectorDialog::addSavedRow(const QString &name)
{
    auto *item = new QListWidgetItem(name, this->list_);
    item->setData(GAME_NAME_ROLE, name);
    item->setData(ADDS_GAME_ROLE, false);
    item->setToolTip("Sends: " + quickgames::gameCommandMessage(
                                     name, quickgames::gameCommand(
                                               this->channel_->getName())));

    // Only the X takes clicks. The rest of the row passes them to the list.
    auto *row = new QWidget();
    row->setMouseTracking(true);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 2, 0);
    rowLayout->addStretch(1);

    auto *remove = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        nullptr, QSize{4, 4});
    const int removeSize = std::max(14, int(16 * this->scale()));
    remove->setFixedSize(removeSize, removeSize);
    remove->setToolTip("Remove " + name + " from your list");
    remove->setColor(this->theme->window.text);
    remove->setCursor(Qt::PointingHandCursor);
    QObject::connect(remove, &Button::leftClicked, this, [this, name] {
        // The button belongs to the list being rebuilt, so wait until it has
        // finished handling this click.
        QTimer::singleShot(0, this, [this, name] {
            this->removeGame(name);
        });
    });
    rowLayout->addWidget(remove, 0, Qt::AlignVCenter);

    this->list_->setItemWidget(item, row);
}

void GameSelectorDialog::addNewGameRow(const QString &name,
                                       const QString &label)
{
    auto *item = new QListWidgetItem(label, this->list_);
    item->setData(GAME_NAME_ROLE, name);
    item->setData(ADDS_GAME_ROLE, true);
    item->setToolTip("Add " + name +
                     " to your list. This doesn't send anything.");
}

void GameSelectorDialog::setStatus(const QString &text)
{
    this->statusLabel_->setText(text);
    this->statusLabel_->setVisible(!text.isEmpty());
}

void GameSelectorDialog::activateItem(QListWidgetItem *item)
{
    if (item == nullptr)
    {
        return;
    }

    const auto name = item->data(GAME_NAME_ROLE).toString();
    if (item->data(ADDS_GAME_ROLE).toBool())
    {
        this->addGame(name);
    }
    else
    {
        this->sendGame(name);
    }
}

void GameSelectorDialog::addGame(const QString &name)
{
    const auto cleaned = name.simplified().left(MAX_NAME_LENGTH);
    if (cleaned.isEmpty() || this->isSaved(cleaned))
    {
        return;
    }

    this->savedGames_.append(cleaned);
    getSettings()->gameSelectorGames = this->savedGames_;
    std::ignore = getSettings()->requestSave();

    this->searchInput_->clear();
    this->searchInput_->setFocus();
    this->setStatus("Added " + cleaned + ". Click it to send it to chat.");
}

void GameSelectorDialog::removeGame(const QString &name)
{
    this->savedGames_.removeAll(name);
    getSettings()->gameSelectorGames = this->savedGames_;
    std::ignore = getSettings()->requestSave();

    this->onSearchTextChanged();
    if (!this->savedGames_.isEmpty())
    {
        this->setStatus("Removed " + name + ".");
    }
}

void GameSelectorDialog::sendGame(const QString &name)
{
    if (this->sinceLastSend_.isValid() &&
        this->sinceLastSend_.elapsed() < RESEND_GUARD_MS)
    {
        return;
    }

    // Never send to a channel other than the one this popup says it sends to
    if (this->split_ == nullptr ||
        this->split_->getSelectedChannel().get() != this->channel_.get())
    {
        this->setStatus("That tab changed channel, so nothing was sent. Click "
                        "in the channel you want, then try again.");
        return;
    }

    const auto message = quickgames::gameCommandMessage(
        name, quickgames::gameCommand(this->channel_->getName()));
    if (message.isEmpty())
    {
        return;
    }

    this->sinceLastSend_.start();
    this->channel_->sendMessage(message);
    quickgames::noteGameUsed(name);

    if (this->pinned_)
    {
        this->setStatus("Sending: " + message);
    }
    else
    {
        this->close();
    }
}

bool GameSelectorDialog::isSaved(const QString &name) const
{
    return this->savedGames_.contains(name, Qt::CaseInsensitive);
}

void GameSelectorDialog::refreshStyle()
{
    const float scale = this->scale();
    auto *app = getApp();

    const auto uiFont = app->getFonts()->getFont(FontStyle::UiMedium, scale);
    this->titleLabel_->setFont(
        app->getFonts()->getFont(FontStyle::UiMediumBold, scale));
    this->channelLabel_->setFont(uiFont);
    this->commandLabel_->setFont(uiFont);
    this->commandInput_->setFont(uiFont);
    this->commandInput_->setFixedHeight(popupControlHeight(uiFont, scale));
    this->searchInput_->setFont(uiFont);
    this->searchInput_->setFixedHeight(popupControlHeight(uiFont, scale));
    this->list_->setFont(uiFont);
    this->statusLabel_->setFont(uiFont);
    this->hintLabel_->setFont(uiFont);

    const int margin = std::max(3, int(6 * scale));
    auto *layout = this->getLayoutContainer()->layout();
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(std::max(2, int(4 * scale)));

    const auto background = this->theme->window.background;
    const auto text = this->theme->window.text;
    auto muted = text;
    muted.setAlpha(160);
    const auto border = this->theme->splits.header.border;
    const auto inputBackground = this->theme->splits.input.background;
    const auto card = this->theme->splits.header.background;
    const auto hover =
        this->theme->isLightTheme() ? card.darker(104) : card.lighter(108);

    this->closeButton_->setColor(text);

    this->setStyleSheet(QString(R"(
            QWidget#GameSelectorRoot {
                background: %1;
            }
            QLabel#GameSelectorTitle {
                color: %2;
            }
            QLabel#GameSelectorMuted {
                color: %3;
            }
            QLineEdit#GameSelectorSearch {
                background: %4;
                color: %2;
                border: 1px solid %5;
                border-radius: %6px;
                padding: 0 %7px;
            }
            QListWidget#GameSelectorList {
                background: %8;
                color: %2;
                border: 1px solid %5;
                border-radius: %6px;
                outline: none;
            }
            QListWidget#GameSelectorList::item {
                padding: %9px %7px;
            }
            QListWidget#GameSelectorList::item:hover,
            QListWidget#GameSelectorList::item:selected {
                background: %10;
                color: %2;
            }
        )")
                            .arg(background.name(), text.name(),
                                 muted.name(QColor::HexArgb),
                                 inputBackground.name(), border.name())
                            .arg(std::max(1, int(2 * scale)))
                            .arg(std::max(4, int(5 * scale)))
                            .arg(card.name())
                            .arg(std::max(2, int(3 * scale)))
                            .arg(hover.name()));
}

}  // namespace chatterino
