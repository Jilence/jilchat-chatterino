// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SearchPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/search/AuthorPredicate.hpp"
#include "messages/search/BadgePredicate.hpp"
#include "messages/search/ChannelPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "messages/search/SubtierPredicate.hpp"
#include "providers/publiclogs/PublicLogs.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"

#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTimeEdit>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>

namespace chatterino {

using namespace literals;
using namespace std::chrono_literals;

namespace {

/// Allows a space after `from:` ("from: name" -> "from:name").
QString joinFromValue(const QString &query)
{
    static const QRegularExpression fromSpace(R"(((?:^|\s)[!\-]?from:)\s+)");
    auto result = query;
    result.replace(fromSpace, u"\\1"_s);
    return result;
}

}  // namespace

ChannelPtr SearchPopup::filter(const QString &text, const QString &channelName,
                               const std::vector<MessagePtr> &snapshot,
                               size_t *matches, const QDateTime &since,
                               const QDateTime &until,
                               const QSet<QString> &excludedChannels)
{
    size_t matchCount = 0;
    ChannelPtr channel(new Channel(channelName, Channel::Type::None));

    auto predicates = parsePredicates(joinFromValue(text));

    // Day separators between results of different days. The ones the history
    // already has are skipped so they don't show up twice or out of place.
    const QLocale locale;
    QDate previousDay;
    const auto isDaySeparator = [&locale](const MessagePtr &message) {
        return message->flags.has(MessageFlag::System) &&
               message->loginName.isEmpty() &&
               locale.toDate(message->messageText, QLocale::LongFormat)
                   .isValid();
    };

    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        MessagePtr message = snapshot[i];
        if (isDaySeparator(message))
        {
            continue;
        }
        if (message->serverReceivedTime.isValid() &&
            ((since.isValid() && message->serverReceivedTime < since) ||
             (until.isValid() && message->serverReceivedTime > until)))
        {
            continue;
        }
        if (!excludedChannels.isEmpty() &&
            excludedChannels.contains(message->channelName.toLower()))
        {
            continue;
        }

        bool accept = true;
        for (const auto &pred : predicates)
        {
            if (!pred->appliesTo(*message))
            {
                accept = false;
                break;
            }
        }

        if (accept)
        {
            if (message->serverReceivedTime.isValid())
            {
                const auto day =
                    message->serverReceivedTime.toLocalTime().date();
                // The first result gets its date too, so every time shown
                // belongs to a date.
                if (!previousDay.isValid() || day != previousDay)
                {
                    channel->addMessage(publiclogs::makeDaySeparator(day),
                                        MessageContext::Repost);
                }
                previousDay = day;
            }

            ++matchCount;
            auto overrideFlags = std::optional<MessageFlags>(message->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            channel->addMessage(message, MessageContext::Repost, overrideFlags);
        }
    }

    if (matches != nullptr)
    {
        *matches = matchCount;
    }
    return channel;
}

SearchPopup::SearchPopup(QWidget *parent, Split *split)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    this->initLayout();
    if (this->split_ && this->split_->getChannelView().hasSelection())
    {
        this->searchInput_->setText(
            this->split_->getChannelView().getSelectedText().trimmed());
        this->searchInput_->selectAll();
    }
    this->resize(400, 600);
    this->addShortcuts();

    this->themeChangedEvent();
}

void SearchPopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](const std::vector<QString> &) -> QString {
             this->searchInput_->setFocus();
             this->searchInput_->selectAll();
             return "";
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"scrollPage", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void SearchPopup::addChannel(ChannelView &channel)
{
    if (this->searchChannels_.empty())
    {
        this->channelView_->setSourceChannel(channel.underlyingChannel());
        this->channelName_ = channel.underlyingChannel()->getName();
    }
    else if (this->searchChannels_.size() == 1)
    {
        this->channelView_->setSourceChannel(
            std::make_shared<Channel>("multichannel", Channel::Type::None));

        auto flags = this->channelView_->getFlags();
        flags.set(MessageElementFlag::ChannelName);
        flags.unset(MessageElementFlag::ModeratorTools);
        this->channelView_->setOverrideFlags(flags);
    }

    this->searchChannels_.append(std::ref(channel));

    this->updateWindowTitle();
    this->rebuildChannelsMenu();
    this->updateLogSearchVisibility();
}

QDateTime SearchPopup::rangeStart() const
{
    const auto days = this->timeRangeCombo_->currentData().toInt();
    if (days < 0)
    {
        return this->rangeFromEdit_->dateTime().toUTC();
    }
    if (days == 0)
    {
        return {};
    }
    return QDateTime::currentDateTimeUtc().addDays(-days);
}

QDateTime SearchPopup::rangeEnd() const
{
    if (this->timeRangeCombo_->currentData().toInt() < 0)
    {
        return this->rangeToEdit_->dateTime().toUTC();
    }
    return {};
}

void SearchPopup::updateCustomRangeVisibility()
{
    const bool custom = this->timeRangeCombo_->currentData().toInt() < 0;
    this->rangeFromEdit_->setVisible(custom);
    this->rangeToEdit_->setVisible(custom);
}

void SearchPopup::rebuildChannelsMenu()
{
    this->channelsMenu_->clear();
    QStringList names;
    names.reserve(this->searchChannels_.size());
    for (const auto &view : std::as_const(this->searchChannels_))
    {
        const auto name = view.get().underlyingChannel()->getName();
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive))
        {
            names.push_back(name);
        }
    }
    names.sort(Qt::CaseInsensitive);

    for (const auto &name : names)
    {
        auto *action = this->channelsMenu_->addAction(name);
        action->setCheckable(true);
        action->setChecked(!this->excludedChannels_.contains(name.toLower()));
        QObject::connect(action, &QAction::toggled, this,
                         [this, name](bool included) {
                             const auto key = name.toLower();
                             if (included)
                             {
                                 this->excludedChannels_.remove(key);
                                 // Its logs weren't loaded; load again.
                                 this->logSearch_.key.clear();
                             }
                             else
                             {
                                 this->excludedChannels_.insert(key);
                             }
                             this->updateChannelsButtonText();
                             this->updateLogSearch();
                             this->search();
                         });
    }

    this->channelsButton_->setVisible(this->searchChannels_.size() > 1);
    this->updateChannelsButtonText();
}

void SearchPopup::updateChannelsButtonText()
{
    const auto actions = this->channelsMenu_->actions();
    const auto total = actions.size();
    qsizetype included = 0;
    for (const auto *action : actions)
    {
        included += action->isChecked() ? 1 : 0;
    }
    this->channelsButton_->setText(
        u"Channels (%1/%2)"_s.arg(included).arg(total));
}

void SearchPopup::goToMessage(const MessagePtr &message)
{
    for (const auto &view : this->searchChannels_)
    {
        const auto type = view.get().underlyingChannel()->getType();
        if (type == Channel::Type::TwitchMentions ||
            type == Channel::Type::TwitchAutomod)
        {
            getApp()->getWindows()->scrollToMessage(message);
            return;
        }

        if (view.get().scrollToMessage(message))
        {
            return;
        }
    }
}

void SearchPopup::goToMessageId(const QString &messageId)
{
    for (const auto &view : this->searchChannels_)
    {
        if (view.get().scrollToMessageId(messageId))
        {
            return;
        }
    }
}

void SearchPopup::updateWindowTitle()
{
    QString historyName;

    if (this->searchChannels_.size() > 1)
    {
        this->setWindowTitle("Searching all open tabs");
        this->searchInput_->setPlaceholderText("Search all open tabs");
        return;
    }
    else if (this->channelName_ == "/automod")
    {
        historyName = "automod";
    }
    else if (this->channelName_ == "/mentions")
    {
        historyName = "mentions";
    }
    else if (this->channelName_ == "/whispers")
    {
        historyName = "whispers";
    }
    else if (this->channelName_.isEmpty())
    {
        historyName = "<empty>'s";
    }
    else
    {
        historyName = QString("%1's").arg(this->channelName_);
    }
    this->setWindowTitle("Searching in " + historyName + " history");
    this->searchInput_->setPlaceholderText("Type to search");
}

void SearchPopup::showEvent(QShowEvent *e)
{
    this->search();
    BaseWindow::showEvent(e);
}

bool SearchPopup::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->searchInput_)
    {
        if (event->type() == QEvent::Resize)
        {
            this->layoutResultCountLabel();
        }
        else if (event->type() == QEvent::KeyPress)
        {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent == QKeySequence::DeleteStartOfWord &&
                this->searchInput_->selectionLength() > 0)
            {
                this->searchInput_->backspace();
                return true;
            }
        }
    }
    return false;
}

void SearchPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->setPalette(getTheme()->palette);
    this->updateResultCountLabelStyle();
}

void SearchPopup::updateResultCountLabelStyle()
{
    if (this->resultCountLabel_ == nullptr)
    {
        return;
    }

    const auto color =
        this->searchInput_ != nullptr
            ? this->searchInput_->palette().color(QPalette::PlaceholderText)
            : getTheme()->palette.color(QPalette::PlaceholderText);
    this->resultCountLabel_->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;")
            .arg(color.name(QColor::HexRgb)));
}

void SearchPopup::search()
{
    if (this->snapshot_.size() == 0)
    {
        this->snapshot_ = this->buildSnapshot();
    }

    const auto *source = &this->snapshot_;
    std::vector<MessagePtr> withLogs;
    if (!this->logSearch_.messages.empty() &&
        this->searchLogsCheckbox_->isChecked() &&
        !this->logSearch_.key.isEmpty() &&
        this->logSearch_.key == this->currentLogKey())
    {
        // Logs first (they're older), without messages the chat already has.
        std::unordered_set<QString> ids;
        ids.reserve(this->snapshot_.size());
        for (const auto &message : this->snapshot_)
        {
            if (!message->id.isEmpty())
            {
                ids.insert(message->id);
            }
        }
        withLogs.reserve(this->logSearch_.messages.size() +
                         this->snapshot_.size());
        for (const auto &message : this->logSearch_.messages)
        {
            if (message->id.isEmpty() || !ids.contains(message->id))
            {
                withLogs.push_back(message);
            }
        }
        withLogs.insert(withLogs.end(), this->snapshot_.begin(),
                        this->snapshot_.end());
        std::ranges::stable_sort(withLogs, [](const auto &a, const auto &b) {
            return a->serverReceivedTime < b->serverReceivedTime;
        });
        source = &withLogs;
    }

    // Searching several channels: by day, then by channel name, then by time
    // (the source is already in chronological order).
    std::vector<MessagePtr> byDayAndChannel;
    if (this->searchChannels_.size() > 1)
    {
        byDayAndChannel = *source;
        std::ranges::stable_sort(
            byDayAndChannel, [](const auto &a, const auto &b) {
                const auto dayA = a->serverReceivedTime.toLocalTime().date();
                const auto dayB = b->serverReceivedTime.toLocalTime().date();
                if (dayA != dayB)
                {
                    return dayA < dayB;
                }
                return a->channelName.compare(b->channelName,
                                              Qt::CaseInsensitive) < 0;
            });
        source = &byDayAndChannel;
    }

    const auto total = source->size();
    size_t matches = 0;
    auto channel =
        filter(this->query(), this->channelName_, *source, &matches,
               this->rangeStart(), this->rangeEnd(), this->excludedChannels_);
    this->channelView_->setChannel(channel);
    this->updateResultCount(matches, total);

    this->lastMatches_ = matches;
    if (!this->logSearch_.key.isEmpty() && this->logStatusLabel_->isVisible())
    {
        this->updateLogSearchStatus();
    }
}

std::vector<std::shared_ptr<TwitchChannel>> SearchPopup::logSearchChannels()
    const
{
    std::vector<std::shared_ptr<TwitchChannel>> channels;
    if (!getSettings()->loadOlderMessagesFromPublicLogs)
    {
        return channels;
    }

    for (const auto &view : this->searchChannels_)
    {
        auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(
            view.get().underlyingChannel());
        if (!twitchChannel ||
            twitchChannel->getType() != Channel::Type::Twitch ||
            this->excludedChannels_.contains(
                twitchChannel->getName().toLower()))
        {
            continue;
        }
        // The same channel can be open in several tabs.
        const bool known = std::ranges::any_of(channels, [&](const auto &c) {
            return c->getName() == twitchChannel->getName();
        });
        if (!known)
        {
            channels.push_back(std::move(twitchChannel));
        }
    }
    return channels;
}

QString SearchPopup::logSearchUser(const QString &query)
{
    static const QRegularExpression fromRegex(
        R"((?:^|\s)from:(?<name>[^\s,]+)(?=\s|$))");

    QString user;
    auto it = fromRegex.globalMatch(joinFromValue(query));
    while (it.hasNext())
    {
        const auto name = it.next().captured("name");
        if (!user.isEmpty() && user.compare(name, Qt::CaseInsensitive) != 0)
        {
            return {};  // several users: not supported
        }
        user = name;
    }
    if (user.startsWith('@'))
    {
        user.remove(0, 1);
    }
    return user;
}

BoolSetting &SearchPopup::searchPublicLogsSetting() const
{
    return this->searchChannels_.size() > 1
               ? getSettings()->searchPublicLogsAllChannels
               : getSettings()->searchPublicLogs;
}

void SearchPopup::updateLogSearchVisibility()
{
    {
        // Show the state remembered for this kind of search without saving
        // it back.
        const QSignalBlocker blocker(this->searchLogsCheckbox_);
        this->searchLogsCheckbox_->setChecked(this->searchPublicLogsSetting());
    }

    const bool available = !this->logSearchChannels().empty();
    this->searchLogsCheckbox_->setVisible(available);
    this->logStatusLabel_->setVisible(available &&
                                      this->searchLogsCheckbox_->isChecked());
    this->updateLogSearch();
}

void SearchPopup::setLogSearchStatus(const QString &status)
{
    this->logStatusLabel_->setText(status);
}

QStringList SearchPopup::logSearchWords(const QString &query)
{
    static const QRegularExpression tokenRegex(R"("[^"]+"|\S+)");

    QStringList words;
    auto it = tokenRegex.globalMatch(joinFromValue(query));
    while (it.hasNext())
    {
        auto token = it.next().captured();
        if (token.startsWith(u'-') || token.startsWith(u'!') ||
            (!token.startsWith(u'"') && token.contains(u':')))
        {
            continue;  // a filter or a negation, not a word to look for
        }
        token.remove(u'"');
        token = token.trimmed().toLower();
        if (!token.isEmpty())
        {
            words.push_back(token);
        }
    }
    return words;
}

QString SearchPopup::currentLogKey() const
{
    const auto query = this->query();
    const auto user = logSearchUser(query).toLower();
    if (!user.isEmpty())
    {
        return u"u:"_s + user;
    }
    if (!this->rangeStart().isValid())
    {
        return {};
    }
    const auto words = logSearchWords(query);
    if (words.isEmpty())
    {
        return {};
    }
    return u"w:"_s + words.join(u'\n');
}

void SearchPopup::updateLogSearchStatus()
{
    const auto &state = this->logSearch_;
    const QLocale locale;
    const auto byDay = state.user.isEmpty();
    const auto unitWord = byDay ? u"days"_s : u"months"_s;
    const auto unitName = [&](LogUnit unit) {
        if (unit.day > 0)
        {
            return locale.toString(QDate(unit.year, unit.month, unit.day),
                                   QLocale::ShortFormat);
        }
        return u"%1 %2"_s.arg(
            locale.standaloneMonthName(unit.month, QLocale::ShortFormat),
            QString::number(unit.year));
    };
    const auto span = [&](LogUnit oldest, LogUnit newest) {
        return oldest == newest
                   ? unitName(newest)
                   : u"%1 – %2"_s.arg(unitName(oldest), unitName(newest));
    };

    size_t total = 0;
    size_t withLogs = 0;
    size_t failed = 0;
    bool busy = false;
    std::optional<LogUnit> newest;
    std::optional<LogUnit> oldest;
    for (const auto &logs : state.channels)
    {
        total += logs.units.size();
        failed += logs.error.isEmpty() ? 0 : 1;
        busy = busy || !logs.done;
        if (!logs.units.empty())
        {
            ++withLogs;
            newest = std::max(newest.value_or(logs.units.front()),
                              logs.units.front());
            oldest =
                std::min(oldest.value_or(logs.units.back()), logs.units.back());
        }
    }

    // The searched period: the chosen time range, otherwise what was found.
    QString dates;
    // The range chosen now: it may be smaller than what was loaded.
    const auto since = this->rangeStart();
    const auto until = this->rangeEnd();
    if (since.isValid() || until.isValid())
    {
        QString from;
        if (since.isValid())
        {
            from = locale.toString(since.toLocalTime().date(),
                                   QLocale::ShortFormat);
        }
        else if (oldest)
        {
            from = unitName(*oldest);
        }
        const auto to = locale.toString(
            until.isValid() ? until.toLocalTime().date() : QDate::currentDate(),
            QLocale::ShortFormat);
        dates = u"%1 – %2"_s.arg(from, to);
    }
    else if (newest && oldest)
    {
        dates = span(*oldest, *newest);
    }

    // Status line, e.g. `"waga": Searching… 34 matches in 13 of 17 channels ·
    // 27.08.26 – 26.09.26`, and "Done" once everything is loaded.
    const auto subject = state.user.isEmpty()
                             ? u"\"%1\""_s.arg(state.words.join(u' '))
                             : state.user;
    QString text = subject + u": "_s;
    text += busy ? u"Searching… "_s : u"Done · "_s;
    if (!busy && total == 0 && failed == 0)
    {
        text += u"no public logs in this time range"_s;
    }
    else
    {
        text += (this->lastMatches_ == 1 ? u"%1 match"_s : u"%1 matches"_s)
                    .arg(locale.toString(qulonglong(this->lastMatches_)));
        if (state.channels.size() > 1)
        {
            text += u" in %1 of %2 channels"_s.arg(withLogs).arg(
                state.channels.size());
        }
        if (!dates.isEmpty())
        {
            text += u" · "_s + dates;
        }
        if (failed > 0)
        {
            text += u" · %1 failed"_s.arg(failed);
        }
    }
    this->setLogSearchStatus(text);

    // Tooltip: one line per channel, most messages first.
    std::vector<const ChannelLogs *> sorted;
    sorted.reserve(state.channels.size());
    for (const auto &logs : state.channels)
    {
        sorted.push_back(&logs);
    }
    std::ranges::stable_sort(sorted, [](const auto *a, const auto *b) {
        return a->messageCount > b->messageCount;
    });

    QString tooltip = u"<table>"_s;
    for (const auto *logs : sorted)
    {
        QString details;
        if (!logs->error.isEmpty())
        {
            details = u"error: %1"_s.arg(logs->error.toHtmlEscaped());
        }
        else if (!logs->listed)
        {
            details = u"loading..."_s;
        }
        else if (logs->units.empty())
        {
            details = u"no logs"_s;
        }
        else
        {
            details = u"%1/%2 %3 · %4 messages · %5"_s.arg(
                QString::number(logs->nextUnit),
                QString::number(logs->units.size()), unitWord,
                locale.toString(qulonglong(logs->messageCount)),
                span(logs->units.back(), logs->units.front()));
        }
        tooltip += u"<tr><td><b>%1</b></td><td>&nbsp;&nbsp;%2</td></tr>"_s.arg(
            logs->channel->getName().toHtmlEscaped(), details);
    }
    tooltip += u"</table>"_s;
    this->logStatusLabel_->setToolTip(tooltip);

    // Update the tooltip while it's shown.
    if (QToolTip::isVisible() && this->logStatusLabel_->underMouse())
    {
        QToolTip::showText(QCursor::pos(), tooltip, this->logStatusLabel_);
    }
}

void SearchPopup::updateLogSearch()
{
    auto channels = this->logSearchChannels();
    const bool enabled =
        !channels.empty() && this->searchLogsCheckbox_->isChecked();
    this->logStatusLabel_->setVisible(enabled);
    if (!enabled)
    {
        return;
    }

    const auto query = this->query();
    const auto key = this->currentLogKey();
    if (key.isEmpty())
    {
        if (!this->rangeStart().isValid())
        {
            this->setLogSearchStatus(
                u"Choose a time range or enter a user to search the "
                u"public logs"_s);
        }
        else
        {
            this->setLogSearchStatus(
                u"Type something to search the public logs"_s);
        }
        return;
    }

    auto &state = this->logSearch_;
    const auto since = this->rangeStart();
    const auto until = this->rangeEnd();
    // Logs are loaded by day/month, so the same days need no new search.
    const auto sameDay = [](const QDateTime &a, const QDateTime &b) {
        return a.isValid() == b.isValid() &&
               (!a.isValid() || a.toUTC().date() == b.toUTC().date());
    };
    if (key == state.key && sameDay(since, state.since) &&
        sameDay(until, state.until))
    {
        return;  // already loaded or loading
    }

    // Start over in every channel.
    ++state.generation;
    state.key = key;
    state.user = logSearchUser(query).toLower();
    state.words = state.user.isEmpty() ? logSearchWords(query) : QStringList{};
    state.since = since;
    state.until = until;
    state.messages.clear();
    state.channels.clear();
    for (auto &channel : channels)
    {
        state.channels.push_back({.channel = std::move(channel)});
    }
    this->updateLogSearchStatus();

    // Responses of a previous search are dropped by publiclogs::get.
    const QPointer<SearchPopup> self(this);
    const auto generation = state.generation;
    const auto stillWanted = [self, generation] {
        return self && generation == self->logSearch_.generation;
    };
    for (size_t i = 0; i < state.channels.size(); ++i)
    {
        publiclogs::get(
            publiclogs::listUrl(state.channels[i].channel->getName(),
                                state.user),
            15000,
            [self, i](const NetworkResult &result) {
                auto &state = self->logSearch_;
                auto &logs = state.channels[i];
                logs.listed = true;
                logs.units = publiclogs::parseLogDates(result);

                // Only the months/days inside the time range (UTC dates).
                const auto first = state.since.isValid()
                                       ? state.since.toUTC().date()
                                       : QDate();
                const auto last = state.until.isValid()
                                      ? state.until.toUTC().date()
                                      : QDate();
                std::erase_if(logs.units, [&](LogUnit unit) {
                    return (first.isValid() && unit.lastDay() < first) ||
                           (last.isValid() && unit.firstDay() > last);
                });
                self->fetchNextLogUnit(i);
            },
            [self, i](const NetworkResult &result) {
                // 404: nothing logged (for this user) in this channel.
                auto &logs = self->logSearch_.channels[i];
                logs.listed = true;
                if (result.status() != 404)
                {
                    qCWarning(chatterinoWidget)
                        << "Failed to list logs for search:"
                        << result.formatError();
                    logs.error = result.formatError();
                }
                logs.done = true;
                self->updateLogSearchStatus();
            },
            stillWanted);
    }
}

void SearchPopup::fetchNextLogUnit(size_t channelIndex)
{
    auto &state = this->logSearch_;
    auto &logs = state.channels[channelIndex];
    if (logs.nextUnit >= logs.units.size())
    {
        logs.done = true;
        this->updateLogSearchStatus();
        return;
    }
    this->updateLogSearchStatus();

    const auto &unit = logs.units[logs.nextUnit];
    const auto &channelName = logs.channel->getName();
    const QPointer<SearchPopup> self(this);
    const auto generation = state.generation;
    publiclogs::get(
        state.user.isEmpty()
            ? publiclogs::channelDayUrl(channelName, unit)
            : publiclogs::userMonthUrl(channelName, state.user, unit),
        60000,
        [self, channelIndex](const NetworkResult &result) {
            const auto &words = self->logSearch_.words;
            auto &logs = self->logSearch_.channels[channelIndex];

            // Channel logs are big: only build the messages that contain all
            // words, the filters run on those afterwards.
            std::function<bool(const QJsonObject &)> containsWords;
            if (!words.isEmpty())
            {
                containsWords = [&words](const QJsonObject &line) {
                    const auto haystack =
                        (line.value("username").toString() + u' ' +
                         line.value("displayName").toString() + u' ' +
                         line.value("text").toString())
                            .toLower();
                    return std::ranges::all_of(words, [&](const auto &word) {
                        return haystack.contains(word);
                    });
                };
            }
            auto built = publiclogs::buildMessages(
                publiclogs::parseLogLines(result), logs.channel.get(), false,
                containsWords);

            logs.messageCount += built.size();
            ++logs.nextUnit;
            auto &messages = self->logSearch_.messages;
            messages.insert(messages.end(),
                            std::make_move_iterator(built.begin()),
                            std::make_move_iterator(built.end()));

            if (!self->searchRefreshTimer_.isActive())
            {
                self->searchRefreshTimer_.start();
            }
            self->fetchNextLogUnit(channelIndex);
        },
        [self, channelIndex](const NetworkResult &result) {
            qCWarning(chatterinoWidget)
                << "Failed to load logs for search:" << result.formatError();
            auto &logs = self->logSearch_.channels[channelIndex];
            logs.error = result.formatError();
            logs.done = true;
            self->updateLogSearchStatus();
        },
        [self, generation] {
            return self && generation == self->logSearch_.generation;
        });
}

QString SearchPopup::query() const
{
    auto user = this->userInput_->text().trimmed();
    if (user.startsWith(u'@'))
    {
        user.remove(0, 1);
    }
    auto text = this->searchInput_->text();
    if (user.isEmpty())
    {
        return text;
    }
    return u"from:%1 %2"_s.arg(user, text);
}

void SearchPopup::refreshSearchKeepingScroll()
{
    auto &scrollbar = this->channelView_->getScrollBar();
    const bool atBottom = scrollbar.isAtBottom();
    // Older results are added above, so keep the distance to the bottom.
    const auto fromBottom =
        scrollbar.getMaximum() - scrollbar.getDesiredValue();

    this->search();

    if (!atBottom)
    {
        scrollbar.setDesiredValue(std::max(
            scrollbar.getMinimum(), scrollbar.getMaximum() - fromBottom));
    }
}

void SearchPopup::updateResultCount(size_t matches, size_t total)
{
    if (this->query().trimmed().isEmpty())
    {
        this->resultCountLabel_->setText(QString::number(total));
        this->resultCountLabel_->setToolTip(
            QString("%1 messages in history").arg(total));
    }
    else
    {
        this->resultCountLabel_->setText(
            QString("%1/%2").arg(matches).arg(total));
        this->resultCountLabel_->setToolTip(
            QString("%1 matching messages out of %2 total")
                .arg(matches)
                .arg(total));
    }

    const QFontMetrics fm(this->resultCountLabel_->font());
    const int counterWidth =
        fm.horizontalAdvance(this->resultCountLabel_->text());
    constexpr int CLEAR_BUTTON_PADDING = 28;
    this->searchInput_->setTextMargins(
        0, 0, counterWidth + CLEAR_BUTTON_PADDING + 4, 0);
    this->layoutResultCountLabel();
}

void SearchPopup::layoutResultCountLabel()
{
    if (this->resultCountLabel_ == nullptr || this->searchInput_ == nullptr)
    {
        return;
    }

    const QFontMetrics fm(this->resultCountLabel_->font());
    const int labelWidth =
        fm.horizontalAdvance(this->resultCountLabel_->text());
    const int labelHeight = fm.height();

    constexpr int CLEAR_BUTTON_PADDING = 28;
    const int x =
        this->searchInput_->width() - CLEAR_BUTTON_PADDING - labelWidth;
    const int y = (this->searchInput_->height() - labelHeight) / 2;

    this->resultCountLabel_->setGeometry(x, y, labelWidth, labelHeight);
}

std::vector<MessagePtr> SearchPopup::buildSnapshot()
{
    if (this->searchChannels_.length() == 1)
    {
        const auto channelPtr = this->searchChannels_.at(0);
        return channelPtr.get().channel()->getMessageSnapshot();
    }

    auto combinedSnapshot = std::vector<std::shared_ptr<const Message>>{};
    for (auto &channel : this->searchChannels_)
    {
        ChannelView &sharedView = channel.get();

        const FilterSetPtr filterSet = sharedView.getFilterSet();
        std::vector<MessagePtr> snapshot =
            sharedView.channel()->getMessageSnapshot();

        for (const auto &message : snapshot)
        {
            if (filterSet &&
                !filterSet->filter(message, sharedView.underlyingChannel()))
            {
                continue;
            }

            combinedSnapshot.push_back(message);
        }
    }

    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  return a->id > b->id;
              });

    auto uniqueIterator =
        std::unique(combinedSnapshot.begin(), combinedSnapshot.end(),
                    [](MessagePtr &a, MessagePtr &b) {
                        return !a->id.isEmpty() && a->id == b->id;
                    });

    combinedSnapshot.erase(uniqueIterator, combinedSnapshot.end());

    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  return a->serverReceivedTime < b->serverReceivedTime;
              });

    return combinedSnapshot;
}

void SearchPopup::initLayout()
{
    {
        auto *layout1 = new QVBoxLayout(this);
        layout1->setContentsMargins(0, 0, 0, 0);
        layout1->setSpacing(0);

        {
            auto *layout2 = new QHBoxLayout();
            layout2->setContentsMargins(8, 8, 8, 8);
            layout2->setSpacing(8);

            {
                this->userInput_ = new QLineEdit(this);
                this->userInput_->setPlaceholderText(u"User"_s);
                this->userInput_->setToolTip(
                    u"Only messages of this user (same as from:<user>)"_s);
                this->userInput_->setClearButtonEnabled(true);
                this->userInput_->setMaximumWidth(160);
                QObject::connect(this->userInput_, &QLineEdit::textChanged,
                                 this, [this] {
                                     this->search();
                                     this->logSearchTimer_.start();
                                 });
                layout2->addWidget(this->userInput_);

                this->searchInput_ = new QLineEdit(this);
                layout2->addWidget(this->searchInput_, 1);

                this->resultCountLabel_ = new QLabel(this->searchInput_);
                this->resultCountLabel_->setAttribute(
                    Qt::WA_TransparentForMouseEvents);
                this->resultCountLabel_->raise();
                this->updateResultCountLabelStyle();

                this->searchInput_->setPlaceholderText("Type to search");
                this->searchInput_->setClearButtonEnabled(true);
                this->searchInput_->findChild<QAbstractButton *>()->setIcon(
                    QPixmap(":/buttons/clearSearch.png"));
                QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                                 this, &SearchPopup::search);
                this->searchInput_->installEventFilter(this);
            }

            {
                this->searchLogsCheckbox_ =
                    new QCheckBox(u"Search public logs"_s, this);
                this->searchLogsCheckbox_->setToolTip(
                    u"Also search the public logs (logs.zonian.dev): all logs "
                    u"of the user in the User field, or without a user "
                    u"the channel logs of the chosen time range. This sends "
                    u"the channel, and the username if given, to that "
                    u"service."_s);
                this->searchLogsCheckbox_->setVisible(false);
                QObject::connect(this->searchLogsCheckbox_, &QCheckBox::toggled,
                                 this, [this](bool checked) {
                                     this->searchPublicLogsSetting() = checked;
                                     this->updateLogSearch();
                                     this->search();
                                 });
                layout2->addWidget(this->searchLogsCheckbox_);
            }

            layout1->addLayout(layout2);
        }

        {
            auto *options = new QHBoxLayout();
            options->setContentsMargins(8, 0, 8, 6);
            options->setSpacing(8);

            options->addWidget(new QLabel(u"Time:"_s, this));
            this->timeRangeCombo_ = new QComboBox(this);
            const std::array<std::pair<const char *, int>, 7> ranges{{
                {"Any time", 0},
                {"Last 24 hours", 1},
                {"Last 7 days", 7},
                {"Last 30 days", 30},
                {"Last 3 months", 91},
                {"Last 6 months", 182},
                {"Last year", 365},
            }};
            for (const auto &[name, days] : ranges)
            {
                this->timeRangeCombo_->addItem(QString::fromUtf8(name), days);
            }
            this->timeRangeCombo_->addItem(u"Custom\u2026"_s, -1);
            QObject::connect(this->timeRangeCombo_,
                             &QComboBox::currentIndexChanged, this, [this] {
                                 this->updateCustomRangeVisibility();
                                 this->updateLogSearch();
                                 this->search();
                             });
            options->addWidget(this->timeRangeCombo_);

            const auto now = QDateTime::currentDateTime();
            const auto makeEdit = [this](const QDateTime &value) {
                auto *edit = new QDateTimeEdit(value, this);
                edit->setCalendarPopup(true);
                // English month and weekday names, like the rest of the UI.
                const QLocale english(QLocale::English);
                edit->setLocale(english);
                edit->calendarWidget()->setLocale(english);
                edit->setDisplayFormat(u"dd.MM.yyyy HH:mm"_s);
                edit->setVisible(false);
                QObject::connect(edit, &QDateTimeEdit::dateTimeChanged, this,
                                 [this] {
                                     this->search();
                                     // Waits for typing to stop like the query.
                                     this->logSearchTimer_.start();
                                 });
                return edit;
            };
            this->rangeFromEdit_ = makeEdit(now.addDays(-7));
            this->rangeToEdit_ = makeEdit(now);
            options->addWidget(this->rangeFromEdit_);
            options->addWidget(this->rangeToEdit_);
            options->addStretch(1);

            this->channelsButton_ = new QToolButton(this);
            this->channelsButton_->setPopupMode(QToolButton::InstantPopup);
            this->channelsButton_->setToolTip(
                u"Choose which channels are searched"_s);
            this->channelsMenu_ = new QMenu(this->channelsButton_);
            this->channelsButton_->setMenu(this->channelsMenu_);
            this->channelsButton_->setVisible(false);
            options->addWidget(this->channelsButton_);

            layout1->addLayout(options);
        }

        {
            this->logStatusLabel_ = new QLabel(this);
            this->logStatusLabel_->setContentsMargins(8, 0, 8, 6);
            this->logStatusLabel_->setVisible(false);
            layout1->addWidget(this->logStatusLabel_);

            this->searchRefreshTimer_.setSingleShot(true);
            this->searchRefreshTimer_.setInterval(250ms);
            QObject::connect(&this->searchRefreshTimer_, &QTimer::timeout, this,
                             &SearchPopup::refreshSearchKeepingScroll);

            this->logSearchTimer_.setSingleShot(true);
            this->logSearchTimer_.setInterval(500ms);
            QObject::connect(&this->logSearchTimer_, &QTimer::timeout, this,
                             &SearchPopup::updateLogSearch);
            QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                             &this->logSearchTimer_,
                             qOverload<>(&QTimer::start));
        }

        {
            this->channelView_ = new ChannelView(
                this, this->split_, ChannelView::Context::Search,
                getSettings()->scrollbackSplitLimit);

            layout1->addWidget(this->channelView_, 1);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    static QRegularExpression predicateRegex(
        R"lit((?<negation>[!\-])?(?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    static QRegularExpression trimQuotationMarksRegex(R"(^"|"$)");

    QRegularExpressionMatchIterator it = predicateRegex.globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");
        bool isNegated = !match.captured("negation").isEmpty();
        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex);

        if (name == "from")
        {
            predicates.push_back(
                std::make_unique<AuthorPredicate>(value, isNegated));
        }
        else if (name == "badge")
        {
            predicates.push_back(
                std::make_unique<BadgePredicate>(value, isNegated));
        }
        else if (name == "subtier")
        {
            predicates.push_back(
                std::make_unique<SubtierPredicate>(value, isNegated));
        }
        else if (name == "has" && value == "link")
        {
            predicates.push_back(std::make_unique<LinkPredicate>(isNegated));
        }
        else if (name == "in")
        {
            predicates.push_back(
                std::make_unique<ChannelPredicate>(value, isNegated));
        }
        else if (name == "is")
        {
            predicates.push_back(
                std::make_unique<MessageFlagsPredicate>(value, isNegated));
        }
        else if (name == "regex")
        {
            predicates.push_back(
                std::make_unique<RegexPredicate>(value, isNegated));
        }
        else
        {
            predicates.push_back(
                std::make_unique<SubstringPredicate>(match.captured()));
        }
    }

    return predicates;
}

}  // namespace chatterino
