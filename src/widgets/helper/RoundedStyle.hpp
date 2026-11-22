// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QProxyStyle>

namespace chatterino {

/// Controls in the spirit of native macOS on top of Fusion: quiet glass
/// surfaces barely lighter than the window with a hairline highlight,
/// capsule buttons, switches for check boxes, thin rounded scroll bars and
/// menus whose selection is an inset rounded pill.
class RoundedStyle : public QProxyStyle
{
public:
    explicit RoundedStyle(QStyle *base);

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter,
                       const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter,
                     const QWidget *widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex *option,
                            QPainter *painter,
                            const QWidget *widget = nullptr) const override;
    QRect subControlRect(ComplexControl control,
                         const QStyleOptionComplex *option,
                         SubControl subControl,
                         const QWidget *widget = nullptr) const override;
};

/// Rounds the corners of every top-level window (menus, popups, dialogs)
/// where the platform supports it (Windows 11). Call once after the
/// QApplication exists.
void installRoundedWindows();

}  // namespace chatterino
