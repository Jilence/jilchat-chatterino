// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QUrl>

#include <memory>
#include <optional>
#include <ostream>
#include <vector>

namespace chatterino {

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
    std::vector<std::shared_ptr<QColor>> extraColors{};

    bool operator==(const HighlightResult &other) const;
    bool operator!=(const HighlightResult &other) const;

    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool full() const;

    friend std::ostream &operator<<(std::ostream &os,
                                    const HighlightResult &result);
};

}  // namespace chatterino
