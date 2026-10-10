// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QElapsedTimer>
#include <QPaintEvent>
#include <QString>
#include <QTimer>

class QLabel;
class QProgressBar;
class QPushButton;

namespace chatterino {

class Split;

class SplitVoiceMessagePanel : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitVoiceMessagePanel(Split *split);

    void refresh();

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateStyleSheets();

    Split *const split_;
    QTimer refreshTimer_;
    QString activeVoiceId_;
    /// Tracks how long the active voice has been paused, to auto-hide after 3s.
    QElapsedTimer pausedSince_;

    QLabel *label_{};
    QProgressBar *progress_{};
    QPushButton *stopButton_{};
};

}  // namespace chatterino
