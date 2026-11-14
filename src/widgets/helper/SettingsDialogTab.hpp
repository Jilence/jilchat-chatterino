// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QIcon>
#include <QPaintEvent>
#include <QWidget>

#include <functional>

namespace chatterino {

class SettingsPage;
class SettingsDialog;

enum class SettingsTabId {
    None,
    General,
    Customization,
    JilChat,
    Accounts,
    Moderation,
    About,
};

class SettingsDialogTab : public BaseWidget
{
    Q_OBJECT

public:
    SettingsDialogTab(SettingsDialog *dialog_,
                      std::function<SettingsPage *()> page_,
                      const QString &name, QString imageFileName,
                      SettingsTabId id);

    /// @param animate Fade to the new look. Off for the search, which
    /// changes the selection while typing.
    void setSelected(bool _selected, bool animate = true);
    SettingsPage *page();
    SettingsPage *createdPage() const;
    SettingsTabId id() const;

    const QString &name() const;

Q_SIGNALS:
    void selectedChanged(bool);

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;

    struct {
        QString labelText;
        QIcon icon;
    } ui_;

    // Parent settings dialog
    SettingsDialog *dialog_{};
    SettingsPage *page_{};
    std::function<SettingsPage *()> lazyPage_;
    SettingsTabId id_;
    QString name_;

    bool selected_ = false;
};

}  // namespace chatterino
