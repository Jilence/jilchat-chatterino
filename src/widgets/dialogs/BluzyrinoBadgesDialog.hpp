// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>

class QVBoxLayout;
class QLabel;

namespace chatterino {

/// "My Bluzyrino badges" — lets the logged-in user choose which donor tier to
/// display. Special badges can be shown/hidden individually once the registry
/// exposes those toggles.
class BluzyrinoBadgesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BluzyrinoBadgesDialog(QWidget *parent = nullptr);

    /// Opens the dialog modelessly, reusing an existing instance if one is
    /// already open.
    static void showDialog(QWidget *parent = nullptr);

private:
    void rebuild();

    QVBoxLayout *contentLayout_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QString userId_;
};

}  // namespace chatterino
