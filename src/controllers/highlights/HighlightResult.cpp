// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightResult.hpp"

namespace chatterino {

HighlightResult HighlightResult::emptyResult()
{
    return {
        .alert = false,
        .sound = {},
        .color = nullptr,
        .showInMentions = false,
        .extraColors = {},
        .matches = {},
    };
}

bool HighlightResult::operator==(const HighlightResult &other) const
{
    if (this->alert != other.alert)
    {
        return false;
    }
    if (this->sound != other.sound)
    {
        return false;
    }

    if (this->color && other.color)
    {
        if (*this->color != *other.color)
        {
            return false;
        }
    }

    if (this->showInMentions != other.showInMentions)
    {
        return false;
    }

    if (this->extraColors.size() != other.extraColors.size())
    {
        return false;
    }
    for (size_t i = 0; i < this->extraColors.size(); ++i)
    {
        if (*this->extraColors[i] != *other.extraColors[i])
        {
            return false;
        }
    }

    if (this->matches != other.matches)
    {
        return false;
    }

    return true;
}

bool HighlightResult::operator!=(const HighlightResult &other) const
{
    return !(*this == other);
}

bool HighlightResult::empty() const
{
    return !this->alert && this->sound.isEmpty() && !this->color &&
           !this->showInMentions;
}

bool HighlightResult::full() const
{
    return this->alert && !this->sound.isEmpty() && this->color &&
           this->showInMentions;
}

std::ostream &operator<<(std::ostream &os, const HighlightResult &result)
{
    os << "Alert: " << (result.alert ? "Yes" : "No") << ", "
       << "Play sound: " << result.sound.toString().toStdString() << ", "
       << "Color: "
       << (result.color
               ? result.color->name(QColor::NameFormat::HexArgb).toStdString()
               : "")
       << ", "
       << "Show in mentions: " << (result.showInMentions ? "Yes" : "No");
    return os;
}

}  // namespace chatterino
