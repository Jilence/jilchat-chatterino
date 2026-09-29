// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "ForwardDecl.hpp"
#include "providers/publiclogs/PublicLogs.hpp"
#include "widgets/BasePopup.hpp"

#include <QDateTime>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QMenu;
class QToolButton;

namespace chatterino {

class Split;
class MessagePredicate;
class TwitchChannel;

class SearchPopup : public BasePopup
{
public:
    SearchPopup(QWidget *parent, Split *split = nullptr);

    virtual void addChannel(ChannelView &channel);
    void goToMessage(const MessagePtr &message);

    void goToMessageId(const QString &messageId);

protected:
    virtual void updateWindowTitle();
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void search();
    /// The search text combined with the user field.
    QString query() const;
    /// Re-runs the search after more logs arrived, keeping the scroll
    /// position unless the view was at the bottom.
    void refreshSearchKeepingScroll();
    void updateResultCount(size_t matches, size_t total);
    void layoutResultCountLabel();
    void updateResultCountLabelStyle();
    void addShortcuts() override;
    std::vector<MessagePtr> buildSnapshot();

    /// The Twitch channels whose public logs can be searched (one, or all
    /// open ones when searching all tabs).
    std::vector<std::shared_ptr<TwitchChannel>> logSearchChannels() const;
    /// The user of the single non-negated `from:` in the query, or empty.
    static QString logSearchUser(const QString &query);
    void updateLogSearchVisibility();
    /// The remembered checkbox state, separate for one and all channels.
    BoolSetting &searchPublicLogsSetting() const;
    /// The plain words of the query (no `name:value` filters, no negations),
    /// lower-case. Used to pre-filter channel logs before building messages.
    static QStringList logSearchWords(const QString &query);
    /// Identifies which logs the current query needs: the `from:` user's logs
    /// ("u:<user>") or the channel logs pre-filtered by the words
    /// ("w:<words>", only with a time range). Empty if the logs can't be
    /// searched.
    QString currentLogKey() const;
    /// Starts or stops loading the logs the query needs.
    void updateLogSearch();
    void fetchNextLogUnit(size_t channelIndex);
    void setLogSearchStatus(const QString &status);
    void updateLogSearchStatus();

    /// @param[out] matches number of matching messages (without separators)
    /// @param since only messages from then on (all if invalid)
    /// @param until only messages up to then (all if invalid)
    /// @param excludedChannels lower-case names of channels to leave out
    static ChannelPtr filter(const QString &text, const QString &channelName,
                             const std::vector<MessagePtr> &snapshot,
                             size_t *matches = nullptr,
                             const QDateTime &since = {},
                             const QDateTime &until = {},
                             const QSet<QString> &excludedChannels = {});

    /// Start of the selected time range (UTC), invalid for "any time".
    QDateTime rangeStart() const;
    /// End of the selected time range (UTC), invalid for "until now".
    QDateTime rangeEnd() const;
    void updateCustomRangeVisibility();
    void rebuildChannelsMenu();
    void updateChannelsButtonText();

    static std::vector<std::unique_ptr<MessagePredicate>> parsePredicates(
        const QString &input);

    std::vector<MessagePtr> snapshot_;
    QLineEdit *searchInput_{};
    /// Limits the search to one user (adds `from:<user>` to the query).
    QLineEdit *userInput_{};
    QCheckBox *searchLogsCheckbox_{};
    QComboBox *timeRangeCombo_{};
    QDateTimeEdit *rangeFromEdit_{};
    QDateTimeEdit *rangeToEdit_{};
    QToolButton *channelsButton_{};
    QMenu *channelsMenu_{};
    /// Lower-case names of channels left out of the search.
    QSet<QString> excludedChannels_;
    QLabel *logStatusLabel_{};
    /// Waits for the user to stop typing before loading logs.
    QTimer logSearchTimer_;

    using LogUnit = publiclogs::LogDate;

    /// The logs searched in one channel.
    struct ChannelLogs {
        std::shared_ptr<TwitchChannel> channel;
        /// Months (user logs) or days (channel logs) with logs, newest first.
        std::vector<LogUnit> units;
        size_t nextUnit = 0;
        bool done = false;
        /// Whether the list of months arrived (it may be empty).
        bool listed = false;
        /// Messages built (after the pre-filter).
        size_t messageCount = 0;
        QString error;
    };

    /// Logs loaded from logs.zonian.dev, in all searched channels at once.
    struct LogSearch {
        /// See currentLogKey().
        QString key;
        /// The `from:` user, or empty when searching channel logs by day.
        QString user;
        /// Words the channel logs are pre-filtered with.
        QStringList words;
        /// The time range the logs were loaded for (invalid: open).
        QDateTime since;
        QDateTime until;
        std::vector<ChannelLogs> channels;
        /// Built messages of all channels; search() sorts them.
        std::vector<MessagePtr> messages;
        /// Bumped to drop responses of a previous user.
        uint64_t generation = 0;
    } logSearch_;

    /// Matches of the last search, shown in the log search status.
    size_t lastMatches_ = 0;

    /// Re-runs the search shortly after logs arrived, so many responses in a
    /// row cause one refresh.
    QTimer searchRefreshTimer_;
    QLabel *resultCountLabel_{};
    ChannelView *channelView_{};
    QString channelName_{};
    Split *split_ = nullptr;
    QList<std::reference_wrapper<ChannelView>> searchChannels_;
};

}  // namespace chatterino
