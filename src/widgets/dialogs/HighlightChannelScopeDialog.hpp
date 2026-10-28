// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/highlights/HighlightChannelScope.hpp"

#include <QDialog>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace chatterino {

/// Asks for the channels a highlight applies in
class HighlightChannelScopeDialog final : public QDialog
{
public:
    HighlightChannelScopeDialog(QWidget *parent, const QString &ruleName,
                                const HighlightChannelScope &existing);

    HighlightChannelScope scope() const;

private:
    HighlightChannelScopeMode mode() const;
    bool addTarget(const HighlightChannelTarget &target);
    void addInputChannels();
    void updateState();

    QComboBox *mode_{};
    QGroupBox *channelsGroup_{};
    QComboBox *targetPlatform_{};
    QLineEdit *channelInput_{};
    QTreeWidget *channels_{};
    QPushButton *remove_{};
    QLabel *summary_{};
};

}  // namespace chatterino
