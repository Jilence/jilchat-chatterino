// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTextEdit;

namespace chatterino {

/// Tells which highlights a message would trigger, and why
class HighlightTesterDialog final : public QDialog
{
public:
    explicit HighlightTesterDialog(QWidget *parent);

private:
    void updateResult();

    QLineEdit *sender_{};
    QLineEdit *channel_{};
    QComboBox *platform_{};
    QPlainTextEdit *message_{};
    QTextEdit *preview_{};
    QPlainTextEdit *explanation_{};
};

}  // namespace chatterino
