// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

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
/// units. Taken from Moltorino.
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

struct HighlightResult {
    HighlightResult(bool _alert, bool _playSound,
                    std::optional<QUrl> _customSoundUrl,
                    std::shared_ptr<QColor> _color, bool _showInMentions);

    static HighlightResult emptyResult();

    bool alert{false};

    bool playSound{false};

    std::optional<QUrl> customSoundUrl{};

    std::shared_ptr<QColor> color{};

    bool showInMentions{false};

    /// Most extra colors collected in `extraColors`. More than the two bands
    /// shown, since colors that end up the same are skipped when painting.
    static constexpr size_t MAX_EXTRA_COLORS = 4;

    /// Colors of further matching highlights, shown as bands when "multiple
    /// highlight bands" is enabled.
    std::vector<std::shared_ptr<QColor>> extraColors;

    /// Where in the message its text matched, if that is to be marked
    std::vector<HighlightMatch> matches;

    bool operator==(const HighlightResult &other) const;
    bool operator!=(const HighlightResult &other) const;

    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool full() const;

    friend std::ostream &operator<<(std::ostream &os,
                                    const HighlightResult &result);
};

}  // namespace chatterino
