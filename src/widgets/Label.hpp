// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Fonts.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <memory>

class QFontMetricsF;

namespace chatterino {

class Paint;

class Label : public BaseWidget
{
public:
    explicit Label(QString text = QString(),
                   FontStyle style = FontStyle::UiMedium);
    explicit Label(BaseWidget *parent, QString text = QString(),
                   FontStyle style = FontStyle::UiMedium);

    const QString &getText() const;
    void setText(const QString &text);

    FontStyle getFontStyle() const;
    void setFontStyle(FontStyle style);

    bool getCentered() const;
    void setCentered(bool centered);

    void setPadding(QMargins padding);

    bool getWordWrap() const;
    void setWordWrap(bool wrap);

    void setShouldElide(bool shouldElide);

    void setElideSuffix(const QString &suffix);

    /// Draws the first `length` characters of the text in `paint` (nullptr:
    /// none). Not with word wrap.
    void setNamePaint(std::shared_ptr<Paint> paint, qsizetype length);
    /// Whether the name is drawn in a paint that moves
    bool hasAnimatedNamePaint() const;
    /// Draws the first `length` characters in `color` (invalid: as the rest)
    /// when they have no paint
    void setNameColor(QColor color, qsizetype length);
    /// Where the name starts in the text (0: at the beginning)
    void setNameStart(qsizetype start);

protected:
    void scaleChangedEvent(float scale_) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    virtual void updateSize();
    QRectF textRect() const;

    QFontMetricsF getFontMetrics() const;

    bool updateElidedText(const QFontMetricsF &fontMetrics, qreal width);

    QString text_;
    FontStyle fontStyle_;
    QSize sizeHint_;
    QSize minimumSizeHint_;

    QMargins basePadding_;

    QMarginsF currentPadding_;

    bool centered_ = false;
    bool wordWrap_ = false;
    bool shouldElide_ = false;

    QString elidedText_;
    QString elideSuffix_;

    std::shared_ptr<Paint> namePaint_;
    qsizetype namePaintLength_ = 0;
    QColor nameColor_;
    qsizetype nameColorLength_ = 0;
    qsizetype nameStart_ = 0;

    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
