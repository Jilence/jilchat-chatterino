// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QJsonArray>
#include <QPoint>
#include <QString>

#include <array>

class QComboBox;
class QListWidget;
class QPushButton;

namespace chatterino {

class Label;

/// The roles roles.tv knows for a user, on the usercard: the channels they
/// are moderator, VIP, founder or artist in, or the users with those roles in
/// their own channel.
class UsercardRolesView : public BaseWidget
{
public:
    explicit UsercardRolesView(QWidget *parent);

    /// Shows the roles of the user. Does nothing if they're shown already.
    void setTarget(const QString &userId, const QString &login);

private:
    static constexpr size_t ROLE_COUNT = 4;

    /// "user" for the roles the user holds, "channel" for their channel.
    QString scope() const;
    void loadSummary();
    void selectRole(size_t role);
    void loadPage();
    void addEntries(const QJsonArray &entries);
    void setStatus(const QString &text);
    void showContextMenu(QPoint pos);

    QString userId_;
    QString login_;
    size_t role_ = 0;
    std::array<int, ROLE_COUNT> totals_{};
    /// Where the next page of the current role starts; empty on the last one.
    QString cursor_;
    /// Bumped with every load, so answers to older ones are dropped.
    int generation_ = 0;

    QComboBox *scope_{};
    std::array<QPushButton *, ROLE_COUNT> tabs_{};
    QListWidget *list_{};
    QPushButton *loadMore_{};
    Label *status_{};
};

}  // namespace chatterino
