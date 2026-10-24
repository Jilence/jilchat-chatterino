// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

class QTreeWidget;

namespace chatterino {

/// The lists of words, phrases or regexes that highlight messages
class HighlightWordListsWidget : public QWidget
{
public:
    HighlightWordListsWidget();

private:
    QTreeWidget *wordLists_{};
    bool refreshingWordLists_{};

    void refreshWordLists();
    void createWordList();
    void importWordList();
    void editSelectedWordList();
    void exportSelectedWordList();
    void removeSelectedWordList();
    int selectedWordListIndex() const;
};

}  // namespace chatterino
