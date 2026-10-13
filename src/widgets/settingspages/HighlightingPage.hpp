// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QAbstractTableModel>
#include <QTimer>

class QPushButton;
class QListWidget;

class QTreeWidget;

namespace chatterino {

class EditableModelView;

class HighlightingPage : public SettingsPage
{
public:
    HighlightingPage();

private:
    enum HighlightTab { Messages = 0, Users = 1, Badges = 2, Blacklist = 3 };

    QTimer disabledUsersChangedTimer_;

    /// The lists of the "Moderator Word Lists" tab
    QTreeWidget *wordLists_{};
    bool refreshingWordLists_{};

    void refreshWordLists();
    void createWordList();
    void importWordList();
    void editSelectedWordList();
    void exportSelectedWordList();
    void removeSelectedWordList();
    int selectedWordListIndex() const;

    void tableCellClicked(const QModelIndex &clicked, EditableModelView *view,
                          HighlightTab tab);
    void openSoundDialog(const QModelIndex &clicked, EditableModelView *view,
                         int soundColumn);
    void openColorDialog(const QModelIndex &clicked, EditableModelView *view,
                         HighlightTab tab);
    /// Asks how the text a phrase matched is to be marked
    void openMatchAppearanceDialog(const QModelIndex &clicked,
                                   EditableModelView *view);
    /// Asks for the channels the highlight of a row applies in
    void openChannelScopeDialog(const QModelIndex &clicked,
                                EditableModelView *view);
    /// Adds the button that asks the same for the selected row
    void addChannelScopeButton(EditableModelView *view);
};

}  // namespace chatterino
