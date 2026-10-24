// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QString>

#include <cstdint>

namespace chatterino {

enum class HighlightMatchSource : std::uint8_t {
    Phrase,
    WordList,
    AutoMod,
};

/// How a matched part of a message is marked
enum class HighlightMatchStyle : std::uint8_t {
    None,
    Outline,
    Fill,
    OutlineAndFill,
    Underline,
};

QString highlightMatchStyleName(HighlightMatchStyle style);
HighlightMatchStyle highlightMatchStyleFromName(const QString &name);

QColor defaultAutoModMatchColor();
/// The marker a new highlight phrase gets
QColor defaultNewHighlightMatchColor();
/// The marker of a phrase from before there were markers: the color the
/// phrase highlights messages with
QColor defaultHighlightMatchColor(QColor messageHighlightColor);

/// A part of a message's text that something matched, counted in UTF-16 code
/// units
struct HighlightMatch {
    qsizetype start{};
    qsizetype length{};
    QColor color;
    QString ruleName;
    QString pattern;
    HighlightMatchSource source{HighlightMatchSource::Phrase};
    HighlightMatchStyle style{HighlightMatchStyle::Outline};
    QString paintID;

    bool operator==(const HighlightMatch &other) const = default;
};

}  // namespace chatterino
