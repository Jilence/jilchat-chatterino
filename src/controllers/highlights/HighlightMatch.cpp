// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightMatch.hpp"

namespace chatterino {

QString highlightMatchStyleName(HighlightMatchStyle style)
{
    switch (style)
    {
        case HighlightMatchStyle::None:
            return QStringLiteral("None");
        case HighlightMatchStyle::Outline:
            return QStringLiteral("Outline");
        case HighlightMatchStyle::Fill:
            return QStringLiteral("Fill");
        case HighlightMatchStyle::OutlineAndFill:
            return QStringLiteral("Outline and fill");
        case HighlightMatchStyle::Underline:
            return QStringLiteral("Underline");
    }
    return QStringLiteral("Outline");
}

HighlightMatchStyle highlightMatchStyleFromName(const QString &name)
{
    if (name.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::None;
    }
    if (name.compare(QStringLiteral("Fill"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::Fill;
    }
    if (name.compare(QStringLiteral("Outline and fill"), Qt::CaseInsensitive) ==
        0)
    {
        return HighlightMatchStyle::OutlineAndFill;
    }
    if (name.compare(QStringLiteral("Underline"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::Underline;
    }
    return HighlightMatchStyle::Outline;
}

QColor defaultAutoModMatchColor()
{
    return {255, 177, 64, 128};
}

QColor defaultNewHighlightMatchColor()
{
    return {255, 255, 255, 235};
}

QColor defaultHighlightMatchColor(QColor messageHighlightColor)
{
    if (!messageHighlightColor.isValid())
    {
        messageHighlightColor = QColor(127, 63, 73);
    }
    messageHighlightColor.setAlpha(150);
    return messageHighlightColor;
}

}  // namespace chatterino
