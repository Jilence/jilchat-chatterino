// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/DimButton.hpp"

namespace chatterino {

class PixmapButton : public DimButton
{
public:
    PixmapButton(BaseWidget *parent = nullptr);

    [[nodiscard]] QPixmap pixmap() const;

    void setPixmap(const QPixmap &pixmap);

    [[nodiscard]] bool marginEnabled() const noexcept;

    void setMarginEnabled(bool enableMargin);

    /// Shows the pixmap cut to a circle, e.g. a profile picture
    void setRound(bool round);

protected:
    void paintContent(QPainter &painter) override;

private:
    QPixmap pixmap_;
    QPixmap resizedPixmap_;
    bool marginEnabled_ = true;
    bool round_ = false;
};

}  // namespace chatterino
