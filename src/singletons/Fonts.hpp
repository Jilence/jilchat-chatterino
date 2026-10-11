// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "pajlada/settings/settinglistener.hpp"

#include <pajlada/signals/signal.hpp>
#include <QFont>
#include <QFontMetrics>
#include <QString>

#include <unordered_map>
#include <vector>

namespace chatterino {

class Settings;
class Paths;

/** @exposeenum c2.FontStyle */
enum class FontStyle : uint8_t {
    Tiny,
    ChatSmall,
    ChatMediumSmall,
    ChatMedium,
    ChatMediumMono,
    ChatMediumStrikethrough,
    ChatMediumBold,
    ChatMediumItalic,
    ChatLarge,
    ChatVeryLarge,

    TimestampMedium,

    UiMedium,
    UiMediumBold,
    UiTabs,

    /// Usernames in chat. Elements keep `ChatMediumBold`; the layout uses
    /// this style for them, so a theme can give usernames their own font.
    ChatUsername,

    EndType,

    ChatStart = ChatSmall,
    ChatEnd = ChatVeryLarge,
};

/// Fonts the current theme asks for. Empty and 0 mean "as set in the
/// settings".
struct ThemeFontOverrides {
    QString chatFamily;
    int chatSize = 0;
    int chatWeight = 0;
    QString usernameFamily;
    int usernameSize = 0;
    int usernameWeight = 0;
    QString interfaceFamily;
    int interfaceSize = 0;

    bool operator==(const ThemeFontOverrides &) const = default;
};

class Fonts final
{
public:
    explicit Fonts(Settings &settings);

    QFont getFont(FontStyle type, float scale);
    QFontMetricsF getFontMetrics(FontStyle type, float scale);

    /// Called by the theme whenever it is loaded.
    static void setThemeOverrides(const ThemeFontOverrides &overrides);

    ~Fonts();
    Fonts(const Fonts &) = delete;
    Fonts &operator=(const Fonts &) = delete;
    Fonts(Fonts &&) = delete;
    Fonts &operator=(Fonts &&) = delete;

    pajlada::Signals::NoArgSignal fontChanged;

private:
    struct FontData {
        FontData(const QFont &_font)
            : font(_font)
            , metrics(_font)
        {
        }

        const QFont font;
        const QFontMetricsF metrics;
    };

    struct ChatFontData {
        float scale;
        bool italic;
    };

    struct UiFontData {
        float size;
        const char *name;
        bool italic;
        int weight;
    };

    /// Forgets all built fonts and tells everyone to lay out again.
    void invalidate();

    FontData &getOrCreateFontData(FontStyle type, float scale);
    static FontData createFontData(FontStyle type, float scale);

    std::vector<std::unordered_map<float, FontData>> fontsByType_;

    pajlada::SettingListener fontChangedListener;
};

}  // namespace chatterino
