// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitVoiceMessagePanel.hpp"

#include "providers/jilchat/JilChatVoice.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>
#include <cmath>

namespace chatterino {

SplitVoiceMessagePanel::SplitVoiceMessagePanel(Split *split)
    : BaseWidget(split)
    , split_(split)
{
    this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);
    this->hide();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(6);

    auto *icon = new QLabel(QStringLiteral(">"), this);
    icon->setFixedWidth(splitHeaderIconColumnWidth(this->scale()));
    icon->setAlignment(Qt::AlignCenter);

    this->label_ = new QLabel(this);
    this->label_->setText(QStringLiteral("Voice message playing"));
    this->label_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    this->progress_ = new QProgressBar(this);
    this->progress_->setRange(0, 1000);
    this->progress_->setTextVisible(false);
    this->progress_->setFixedHeight(6);
    this->progress_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                   QSizePolicy::Fixed);

    this->stopButton_ = new QPushButton(QStringLiteral("X"), this);
    this->stopButton_->setFlat(true);
    this->stopButton_->setCursor(Qt::PointingHandCursor);
    this->stopButton_->setFocusPolicy(Qt::NoFocus);
    this->stopButton_->setToolTip(QStringLiteral("Stop voice message"));
    QObject::connect(this->stopButton_, &QPushButton::clicked, this, [this] {
        jilchat::stopVoiceMessage(this->activeVoiceId_);
        this->activeVoiceId_.clear();
        this->hide();
    });

    layout->addWidget(icon, 0, Qt::AlignVCenter);
    layout->addWidget(this->label_, 0, Qt::AlignVCenter);
    layout->addWidget(this->progress_, 1, Qt::AlignVCenter);
    layout->addWidget(this->stopButton_, 0, Qt::AlignVCenter);

    this->refreshTimer_.setInterval(200);
    this->refreshTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });
    this->refreshTimer_.start();

    this->themeChangedEvent();
    this->scaleChangedEvent(this->scale());
}

void SplitVoiceMessagePanel::refresh()
{
    this->activeVoiceId_ = jilchat::getActiveVoiceId();
    if (this->activeVoiceId_.isEmpty())
    {
        this->hide();
        return;
    }

    const auto progress = jilchat::getVoiceProgress(this->activeVoiceId_);
    this->progress_->setValue(
        static_cast<int>(std::lround(std::clamp(progress, 0.0, 1.0) * 1000)));

    if (!this->isVisible())
    {
        this->show();
    }
}

void SplitVoiceMessagePanel::updateStyleSheets()
{
    if (this->theme == nullptr)
    {
        return;
    }

    const auto text = this->theme->splits.header.text.name(QColor::HexArgb);
    const auto accent = QColor(145, 66, 255).name(QColor::HexArgb);
    const auto bg = this->theme->splits.background.darker(145).name(
        QColor::HexArgb);

    if (this->label_ != nullptr)
    {
        this->label_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }").arg(text));
    }

    if (this->progress_ != nullptr)
    {
        this->progress_->setStyleSheet(
            QStringLiteral(
                "QProgressBar { border: none; background: %1; }"
                "QProgressBar::chunk { background: %2; }")
                .arg(bg, accent));
    }

    if (this->stopButton_ != nullptr)
    {
        this->stopButton_->setStyleSheet(
            QStringLiteral("QPushButton { color: %1; border: none; "
                           "padding: 2px 6px; font-weight: 700; } "
                           "QPushButton:hover { color: %2; }")
                .arg(text, accent));
    }
}

void SplitVoiceMessagePanel::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateStyleSheets();
}

void SplitVoiceMessagePanel::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int fs = static_cast<int>(9 * std::max(1.0f, scale));
    QFont font = this->font();
    font.setPointSize(std::max(8, fs));
    font.setBold(true);

    if (this->label_ != nullptr)
    {
        this->label_->setFont(font);
    }

    if (this->stopButton_ != nullptr)
    {
        this->stopButton_->setFont(font);
    }
}

void SplitVoiceMessagePanel::paintEvent(QPaintEvent * /*event*/)
{
    if (this->theme == nullptr)
    {
        return;
    }

    QPainter painter(this);
    QColor bg = this->theme->splits.background;
    QColor border = this->theme->splits.header.border;
    if (this->split_->hasFocus())
    {
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), bg);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
}

}  // namespace chatterino
