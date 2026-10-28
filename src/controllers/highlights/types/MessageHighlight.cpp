// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/types/MessageHighlight.hpp"

#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"

#include <QIcon>
#include <QStringBuilder>

namespace chatterino::highlights {

MessageHighlight::MessageHighlight(QStringView _id)
    : id(_id.toString())
{
    this->rebuildInternalRegularExpression();
}

bool MessageHighlight::isRegex() const
{
    return this->regex.value_or(false);
}

void MessageHighlight::setRegex(std::optional<bool> newValue)
{
    this->regex = newValue;

    this->rebuildInternalRegularExpression();
}

bool MessageHighlight::isCaseSensitive() const
{
    return this->caseSensitive.value_or(false);
}

void MessageHighlight::setCaseSensitive(std::optional<bool> newValue)
{
    this->caseSensitive = newValue;
    this->rebuildInternalRegularExpression();
}

HighlightCheck MessageHighlight::buildCheck() const
{
    using H = std::remove_pointer_t<decltype(this)>;
    using Params = HighlightCheckParams;

    if (!this->isValid())
    {
        return {};
    }

    return {
        [highlight = *this, scope = this->extras.getChannelScope()](
            const Params &p) -> std::optional<HighlightResult> {
            if (p.self)
            {
                // Phrase checks should ignore highlights from the user
                return std::nullopt;
            }

            if (!scope.appliesTo(p.platform, p.channelName))
            {
                return std::nullopt;
            }

            if (!highlight.isMatch(p.originalMessage))
            {
                return std::nullopt;
            }

            auto result = highlight.outcome.makeSimpleResult<H>();

            const auto style = highlight.getMatchStyle();
            if (style != HighlightMatchStyle::None)
            {
                const auto color = highlight.getMatchColor();
                for (const auto &[start, length] :
                     highlight.findMatches(p.originalMessage))
                {
                    result.matches.push_back({
                        .start = start,
                        .length = length,
                        .color = color,
                        .ruleName = highlight.getPattern(),
                        .pattern = highlight.getPattern(),
                        .source = HighlightMatchSource::Phrase,
                        .style = style,
                        .paintID = {},
                    });
                }
            }

            return result;
        },
    };
}

HighlightMatchStyle MessageHighlight::getMatchStyle() const
{
    return highlightMatchStyleFromName(this->extras.matchStyle);
}

QColor MessageHighlight::getMatchColor() const
{
    QColor color(this->extras.matchColor);
    // a fully transparent marker could not be seen, so it counts as unset
    if (color.isValid() && color.alpha() > 0)
    {
        return color;
    }
    // white, so the marker stands out from the background color
    return defaultNewHighlightMatchColor();
}

std::vector<std::pair<qsizetype, qsizetype>> MessageHighlight::findMatches(
    const QString &subject) const
{
    constexpr size_t MAX_MATCHES = 128;

    std::vector<std::pair<qsizetype, qsizetype>> matches;
    if (!this->isValid())
    {
        return matches;
    }

    auto it = this->regexPattern.globalMatch(subject);
    while (it.hasNext() && matches.size() < MAX_MATCHES)
    {
        const auto match = it.next();
        auto start = match.capturedStart();
        auto end = match.capturedEnd();
        // a phrase is matched together with the whitespace around it
        while (start < end && subject.at(start).isSpace())
        {
            ++start;
        }
        while (end > start && subject.at(end - 1).isSpace())
        {
            --end;
        }
        if (end > start)
        {
            matches.emplace_back(start, end - start);
        }
    }
    return matches;
}

void MessageHighlight::rebuildInternalRegularExpression()
{
    if (this->isRegex())
    {
        this->regexPattern.setPattern(this->getPattern());
    }
    else
    {
        this->regexPattern.setPattern(
            REGEX_START_BOUNDARY %
            QRegularExpression::escape(this->getPattern()) %
            REGEX_END_BOUNDARY);
    }

    if (this->isCaseSensitive())
    {
        this->regexPattern.setPatternOptions(
            QRegularExpression::UseUnicodePropertiesOption);
    }
    else
    {
        this->regexPattern.setPatternOptions(
            QRegularExpression::UseUnicodePropertiesOption |
            QRegularExpression::CaseInsensitiveOption);
    }
}

QDebug operator<<(QDebug dbg, const MessageHighlight &v)
{
    dbg.nospace() << "MessageHighlight("
                  << "name:" << v.name << ',' << "pattern:" << v.pattern << ','
                  << "patternRegex:" << v.regexPattern << ','
                  << "enabled:" << v.enabled << ',' << "isRegex:" << v.regex
                  << ',' << "isCaseSensitive:" << v.caseSensitive << ','
                  << "outcome:" << v.outcome << ')';

    return dbg;
}

}  // namespace chatterino::highlights
