// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "providers/publiclogs/PublicLogs.hpp"
#include "widgets/BaseWidget.hpp"

#include <QString>

#include <vector>

class QComboBox;
class QLineEdit;
class QPushButton;

namespace chatterino {

class ChannelView;
class Label;
class LabelButton;
class Split;

/// The public logs of one user in one channel, on the usercard: a month at a
/// time, with a filter for the month and a search over the whole history.
class UsercardLogsView : public BaseWidget
{
public:
    UsercardLogsView(Split *split, QWidget *parent);

    /// Shows the logs of `user` in `channel`, starting with the newest month.
    /// Does nothing if they're shown already.
    void setTarget(const ChannelPtr &channel, const QString &user);

private:
    void loadMonths();
    void loadMonth(int index);
    void searchAllHistory();
    void setLines(const QJsonArray &lines, bool newestFirst);
    /// Shows the messages matching the search field.
    void applyFilter();
    void setStatus(const QString &text);
    void leaveAllHistory();
    void updateButtons();

    /// The channel the user was opened in.
    ChannelPtr channel_;
    /// Holds the messages shown in view_.
    ChannelPtr viewChannel_;
    QString user_;
    /// Newest first, like the entries of the combo box.
    std::vector<publiclogs::LogDate> months_;
    /// Oldest first.
    std::vector<MessagePtr> messages_;
    bool allHistory_ = false;
    /// Bumped with every load, so answers to older ones are dropped.
    int generation_ = 0;

    LabelButton *older_{};
    LabelButton *newer_{};
    QComboBox *period_{};
    Label *count_{};
    QLineEdit *search_{};
    QPushButton *searchAll_{};
    ChannelView *view_{};
    Label *status_{};
};

}  // namespace chatterino
