// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/UsercardLogsView.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Label.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {

using namespace literals;

namespace {

constexpr int TIMEOUT_MS = 20000;
/// How many matches a search over the whole history asks for.
constexpr int SEARCH_LIMIT = 1000;

QString monthLabel(publiclogs::LogDate month)
{
    return u"%1 '%2"_s.arg(
        QLocale().monthName(month.month, QLocale::ShortFormat),
        QString::number(month.year % 100).rightJustified(2, u'0'));
}

/// The month spelled out, for the opened list.
QString longMonthLabel(publiclogs::LogDate month)
{
    return u"%1 %2"_s.arg(
        QLocale().standaloneMonthName(month.month, QLocale::LongFormat),
        QString::number(month.year));
}

/// Shows the short label of the current item while closed and the long ones
/// in the list, which never reaches below the window it is in.
class PeriodComboBox : public QComboBox
{
public:
    static constexpr int SHORT_LABEL_ROLE = Qt::UserRole + 1;

    using QComboBox::QComboBox;

    void showPopup() override
    {
        const auto rowHeight = std::max(1, this->view()->sizeHintForRow(0));
        const auto *window = this->window();
        const auto top = this->mapToGlobal(QPoint(0, this->height())).y();
        const auto space = std::max(
            3 * rowHeight,
            window->mapToGlobal(QPoint(0, window->height())).y() - top);
        this->setMaxVisibleItems(space / rowHeight);
        this->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        this->view()->setMinimumWidth(
            this->view()->sizeHintForColumn(0) +
            this->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8);
        QComboBox::showPopup();

        // Keep the list right below the box and inside the window, whatever
        // the style made of it.
        auto *popup = this->view()->window();
        auto geometry = popup->geometry();
        geometry.setHeight(std::min(geometry.height(), space));
        geometry.moveTop(top);
        popup->setGeometry(geometry);
        this->view()->scrollTo(this->view()->currentIndex());
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        this->initStyleOption(&option);
        if (const auto shortLabel = this->currentData(SHORT_LABEL_ROLE);
            shortLabel.isValid())
        {
            option.currentText = shortLabel.toString();
        }
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

QDate messageDay(const MessagePtr &message)
{
    return message->serverReceivedTime.toLocalTime().date();
}

}  // namespace

UsercardLogsView::UsercardLogsView(Split *split, QWidget *parent)
    : BaseWidget(parent)
    , period_(new PeriodComboBox(this))
    , search_(new QLineEdit(this))
    , view_(new ChannelView(this, split, ChannelView::Context::UserCard,
                            getSettings()->scrollbackUsercardLimit))
{
    // Frameless popups drag the window from widgets without mouse tracking.
    this->setMouseTracking(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *periodRow = new QHBoxLayout();
    this->older_ = new LabelButton(u"◀"_s, this, QSize{8, 2});
    this->older_->setToolTip("Older period");
    this->newer_ = new LabelButton(u"▶"_s, this, QSize{8, 2});
    this->newer_->setToolTip("Newer period");
    // A plain list instead of the style's menu, which can't scroll.
    this->period_->setStyleSheet("QComboBox { combobox-popup: 0; }");
    // Sized for the short labels; the list gets as wide as the long ones.
    this->period_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    this->period_->setMinimumContentsLength(7);
    this->period_->setToolTip("Log period");
    this->count_ = new Label(this);
    auto *openWeb = new QPushButton("Open on web", this);
    periodRow->addWidget(this->older_);
    periodRow->addWidget(this->period_);
    periodRow->addWidget(this->newer_);
    periodRow->addWidget(this->count_);
    periodRow->addStretch(1);
    periodRow->addWidget(openWeb);
    layout->addLayout(periodRow);

    auto *searchRow = new QHBoxLayout();
    this->search_->setPlaceholderText("Search this period");
    this->search_->setClearButtonEnabled(true);
    this->search_->findChild<QAbstractButton *>()->setIcon(
        QPixmap(":/buttons/clearSearch.png"));
    this->searchAll_ = new QPushButton("Search all history", this);
    searchRow->addWidget(this->search_, 1);
    searchRow->addWidget(this->searchAll_);
    layout->addLayout(searchRow);

    this->view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(this->view_, 1);

    this->status_ = new Label(this);
    this->status_->setCentered(true);
    this->status_->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Expanding);
    this->status_->hide();
    layout->addWidget(this->status_, 1);

    QObject::connect(this->older_, &Button::leftClicked, this, [this] {
        this->period_->setCurrentIndex(this->period_->currentIndex() + 1);
    });
    QObject::connect(this->newer_, &Button::leftClicked, this, [this] {
        this->period_->setCurrentIndex(this->period_->currentIndex() - 1);
    });
    QObject::connect(this->period_, &QComboBox::currentIndexChanged, this,
                     [this](int index) {
                         const auto month = this->period_->itemData(index);
                         if (month.isValid())
                         {
                             this->leaveAllHistory();
                             this->loadMonth(month.toInt());
                         }
                     });
    QObject::connect(openWeb, &QPushButton::clicked, this, [this] {
        if (!this->channel_)
        {
            return;
        }
        QUrl url("https://tv.supa.sh/logs");
        QUrlQuery query;
        query.addQueryItem("c", this->channel_->getName());
        query.addQueryItem("u", this->user_);
        url.setQuery(query);
        QDesktopServices::openUrl(url);
    });
    QObject::connect(this->search_, &QLineEdit::textChanged, this, [this] {
        this->applyFilter();
    });
    QObject::connect(this->search_, &QLineEdit::returnPressed, this, [this] {
        this->searchAllHistory();
    });
    QObject::connect(this->searchAll_, &QPushButton::clicked, this, [this] {
        this->searchAllHistory();
    });
}

void UsercardLogsView::setTarget(const ChannelPtr &channel, const QString &user)
{
    if (this->channel_ == channel && this->user_ == user)
    {
        return;
    }
    this->channel_ = channel;
    this->user_ = user;
    {
        const QSignalBlocker blocker(this->search_);
        this->search_->clear();
    }
    this->loadMonths();
}

void UsercardLogsView::loadMonths()
{
    const auto generation = ++this->generation_;
    this->months_.clear();
    this->messages_.clear();
    this->allHistory_ = false;
    {
        const QSignalBlocker blocker(this->period_);
        this->period_->clear();
    }
    this->updateButtons();
    this->setStatus("Finding available logs...");

    const QPointer<UsercardLogsView> self(this);
    publiclogs::get(
        publiclogs::listUrl(this->channel_->getName(), this->user_), TIMEOUT_MS,
        [self](const NetworkResult &result) {
            self->months_ = publiclogs::parseLogDates(result);
            if (self->months_.empty())
            {
                self->setStatus(
                    "No logs were found for this user in this channel.");
                return;
            }
            {
                const QSignalBlocker blocker(self->period_);
                for (int i = 0; std::cmp_less(i, self->months_.size()); ++i)
                {
                    const auto month = self->months_[i];
                    self->period_->addItem(longMonthLabel(month), i);
                    self->period_->setItemData(
                        i, monthLabel(month), PeriodComboBox::SHORT_LABEL_ROLE);
                }
            }
            self->loadMonth(0);
        },
        [self](const NetworkResult &result) {
            // The service answers 404 for users and channels it doesn't log.
            self->setStatus(
                result.status() == 404
                    ? u"No logs were found for this user in this channel."_s
                    : u"Logs couldn't be loaded: %1"_s.arg(
                          result.formatError()));
        },
        [self, generation] {
            return self && self->generation_ == generation;
        });
}

void UsercardLogsView::loadMonth(int index)
{
    if (index < 0 || std::cmp_greater_equal(index, this->months_.size()))
    {
        return;
    }
    const auto generation = ++this->generation_;
    this->messages_.clear();
    this->updateButtons();
    this->setStatus("Loading logs...");

    const QPointer<UsercardLogsView> self(this);
    publiclogs::get(
        publiclogs::userMonthUrl(this->channel_->getName(), this->user_,
                                 this->months_[index]),
        TIMEOUT_MS,
        [self](const NetworkResult &result) {
            self->setLines(publiclogs::parseLogLines(result), false);
        },
        [self](const NetworkResult &result) {
            self->setStatus(
                u"Logs couldn't be loaded: %1"_s.arg(result.formatError()));
        },
        [self, generation] {
            return self && self->generation_ == generation;
        });
}

void UsercardLogsView::searchAllHistory()
{
    const auto needle = this->search_->text().trimmed();
    if (needle.isEmpty() || this->months_.empty())
    {
        this->search_->setFocus();
        return;
    }
    const auto generation = ++this->generation_;
    if (!this->allHistory_)
    {
        this->allHistory_ = true;
        const QSignalBlocker blocker(this->period_);
        this->period_->insertItem(0, "All history");
        this->period_->setCurrentIndex(0);
    }
    this->messages_.clear();
    this->updateButtons();
    this->setStatus("Searching all history...");

    const QPointer<UsercardLogsView> self(this);
    publiclogs::get(
        publiclogs::userSearchUrl(this->channel_->getName(), this->user_,
                                  needle, SEARCH_LIMIT),
        TIMEOUT_MS,
        [self](const NetworkResult &result) {
            self->setLines(publiclogs::parseLogLines(result), true);
        },
        [self](const NetworkResult &result) {
            self->setStatus(
                u"Logs couldn't be searched: %1"_s.arg(result.formatError()));
        },
        [self, generation] {
            return self && self->generation_ == generation;
        });
}

void UsercardLogsView::leaveAllHistory()
{
    if (!this->allHistory_)
    {
        return;
    }
    this->allHistory_ = false;
    const QSignalBlocker blocker(this->period_);
    this->period_->removeItem(0);
}

void UsercardLogsView::setLines(const QJsonArray &lines, bool newestFirst)
{
    this->messages_.clear();
    // Built like chat messages, so they look like the rest of the usercard.
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->channel_.get()))
    {
        this->messages_ =
            publiclogs::buildMessages(lines, twitchChannel, newestFirst);
    }
    this->applyFilter();
}

void UsercardLogsView::applyFilter()
{
    const auto needle = this->search_->text().trimmed();

    std::vector<MessagePtr> matching;
    for (const auto &message : this->messages_)
    {
        if (needle.isEmpty() ||
            message->messageText.contains(needle, Qt::CaseInsensitive))
        {
            matching.push_back(message);
        }
    }

    // The view only holds so many messages; keep the newest ones.
    const auto limit = static_cast<size_t>(
        std::max(1, std::min(getSettings()->scrollbackUsercardLimit.getValue(),
                             getSettings()->scrollbackSplitLimit.getValue())));
    const auto shown = matching.size();
    const auto first = shown > limit ? shown - limit : 0;

    // A new channel every time: the view then starts at the newest message
    // again, wherever it was scrolled to before.
    this->viewChannel_ =
        std::make_shared<TwitchChannel>(this->channel_->getName());
    QDate previousDay;
    for (auto i = first; i < shown; ++i)
    {
        const auto &message = matching[i];
        if (const auto day = messageDay(message);
            day.isValid() && day != previousDay)
        {
            this->viewChannel_->addMessage(publiclogs::makeDaySeparator(day),
                                           MessageContext::Repost);
            previousDay = day;
        }
        this->viewChannel_->addMessage(message, MessageContext::Repost);
    }

    this->view_->setChannel(this->viewChannel_);
    this->view_->setSourceChannel(this->channel_);

    const auto total = this->messages_.size();
    this->count_->setText(shown == total
                              ? u"%1 messages"_s.arg(total)
                              : u"%1 of %2 messages"_s.arg(shown).arg(total));

    if (shown > 0)
    {
        this->setStatus({});
    }
    else if (needle.isEmpty())
    {
        this->setStatus("No messages in this period.");
    }
    else
    {
        this->setStatus(u"No messages matched \"%1\"."_s.arg(needle));
    }
    this->updateButtons();
}

void UsercardLogsView::setStatus(const QString &text)
{
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
    this->view_->setVisible(text.isEmpty());
    if (!text.isEmpty() && this->messages_.empty())
    {
        this->count_->setText({});
    }
}

void UsercardLogsView::updateButtons()
{
    const auto index = this->period_->currentIndex();
    const bool hasMonths = !this->months_.empty();
    this->period_->setEnabled(hasMonths);
    this->search_->setEnabled(hasMonths);
    this->searchAll_->setEnabled(hasMonths);
    // "All history" isn't part of the months to step through.
    this->older_->setEnabled(hasMonths && !this->allHistory_ &&
                             index + 1 < this->period_->count());
    this->newer_->setEnabled(hasMonths && !this->allHistory_ && index > 0);
}

}  // namespace chatterino
