// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Fonts.hpp"

#include "Application.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QDebug>
#include <qfont.h>
#include <qfontdatabase.h>
#include <QFontDatabase>
#include <QtGlobal>

namespace {

using namespace chatterino;

int getUsernameBoldness()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

    static constexpr std::array<std::array<int, 2>, 9> legacyToOpenTypeMap{{
        {0, QFont::Thin},
        {12, QFont::ExtraLight},
        {25, QFont::Light},
        {50, QFont::Normal},
        {57, QFont::Medium},
        {63, QFont::DemiBold},
        {75, QFont::Bold},
        {81, QFont::ExtraBold},
        {87, QFont::Black},
    }};

    const int target = getSettings()->boldScale.getValue();

    int result = QFont::Medium;
    int closestDist = INT_MAX;

    for (const auto [weightOld, weightNew] : legacyToOpenTypeMap)
    {
        const int dist = qAbs(weightOld - target);
        if (dist < closestDist)
        {
            result = weightNew;
            closestDist = dist;
        }
        else
        {
            break;
        }
    }

    return result;
#else
    return getSettings()->boldScale.getValue();
#endif
}

/// What the theme wants, see Fonts::setThemeOverrides.
ThemeFontOverrides themeFonts;
/// The one Fonts instance, to rebuild its fonts when the theme changes.
Fonts *fontsInstance = nullptr;

/// Registers the Gabarito faces that come with the program, so themes can use
/// them without the font being installed.
void registerBundledFonts()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    for (const auto *face :
         {"Regular", "Medium", "SemiBold", "Bold", "ExtraBold", "Black"})
    {
        const auto path = QStringLiteral(":/fonts/Gabarito/Gabarito-%1.ttf")
                              .arg(QLatin1StringView(face));
        if (QFontDatabase::addApplicationFont(path) < 0)
        {
            qWarning() << "Could not load the bundled font face" << path;
        }
    }
}

float fontSize(FontStyle style)
{
    auto chatSize = [] {
        if (themeFonts.chatSize > 0)
        {
            return static_cast<float>(themeFonts.chatSize);
        }
        return static_cast<float>(getSettings()->chatFontSize);
    };
    switch (style)
    {
        case FontStyle::ChatSmall:
            return 0.6F * chatSize();
        case FontStyle::ChatMediumSmall:
            return 0.8F * chatSize();
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatMediumItalic:
        case FontStyle::ChatMediumMono:
        case FontStyle::ChatMediumStrikethrough:
        case FontStyle::TimestampMedium:
            return chatSize();
        case FontStyle::ChatUsername:
            return themeFonts.usernameSize > 0
                       ? static_cast<float>(themeFonts.usernameSize)
                       : chatSize();
        case FontStyle::ChatLarge:
            return 1.2F * chatSize();
        case FontStyle::ChatVeryLarge:
            return 1.4F * chatSize();

        case FontStyle::Tiny:
            return 8;
        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return themeFonts.interfaceSize > 0
                       ? static_cast<float>(themeFonts.interfaceSize)
                       : 9.F;
    }

    assert(false);
    return 9;
}

int fontWeight(FontStyle style)
{
    switch (style)
    {
        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumMono:
        case FontStyle::ChatMediumStrikethrough:
        case FontStyle::ChatMediumItalic:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
            return themeFonts.chatWeight > 0
                       ? themeFonts.chatWeight
                       : getSettings()->chatFontWeight.getValue();

        case FontStyle::ChatMediumBold:
            return getUsernameBoldness();

        case FontStyle::ChatUsername:
            return themeFonts.usernameWeight > 0 ? themeFonts.usernameWeight
                                                 : getUsernameBoldness();

        case FontStyle::Tiny:
        case FontStyle::UiMedium:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return QFont::Normal;

        case FontStyle::UiMediumBold:
            return QFont::Bold;
    }

    assert(false);
    return QFont::Normal;
}

bool isItalic(FontStyle style)
{
    switch (style)
    {
        case FontStyle::Tiny:
        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumMono:
        case FontStyle::ChatMediumStrikethrough:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatUsername:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return false;

        case FontStyle::ChatMediumItalic:
            return true;
    }

    assert(false);
    return false;
}

QString fontFamily(FontStyle style)
{
    switch (style)
    {
        case FontStyle::Tiny:
            return QStringLiteral("Monospace");

        case FontStyle::ChatUsername:
            if (!themeFonts.usernameFamily.isEmpty())
            {
                return themeFonts.usernameFamily;
            }
            [[fallthrough]];
        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumStrikethrough:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatMediumItalic:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
            return themeFonts.chatFamily.isEmpty()
                       ? getSettings()->chatFontFamily.getValue()
                       : themeFonts.chatFamily;
        case FontStyle::ChatMediumMono:
            QFontDatabase::applicationFontFamilies(getApp()->monoFontId).at(0);
        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return themeFonts.interfaceFamily.isEmpty()
                       ? QStringLiteral(DEFAULT_FONT_FAMILY)
                       : themeFonts.interfaceFamily;
    }

    assert(false);
    return QStringLiteral(DEFAULT_FONT_FAMILY);
}

}  // namespace

namespace chatterino {

Fonts::Fonts(Settings &settings)
{
    this->fontsByType_.resize(size_t(FontStyle::EndType));

    fontsInstance = this;
    registerBundledFonts();

    this->fontChangedListener.setCB([this] {
        this->invalidate();
    });
    this->fontChangedListener.addSetting(settings.chatFontFamily);
    this->fontChangedListener.addSetting(settings.chatFontSize);
    this->fontChangedListener.addSetting(settings.chatFontWeight);
    this->fontChangedListener.addSetting(settings.boldScale);
}

Fonts::~Fonts()
{
    if (fontsInstance == this)
    {
        fontsInstance = nullptr;
    }
}

void Fonts::setThemeOverrides(const ThemeFontOverrides &overrides)
{
    if (themeFonts == overrides)
    {
        return;
    }
    themeFonts = overrides;
    if (fontsInstance != nullptr)
    {
        fontsInstance->invalidate();
    }
}

void Fonts::invalidate()
{
    assertInGuiThread();

    for (auto &map : this->fontsByType_)
    {
        map.clear();
    }
    this->fontChanged.invoke();
}

QFont Fonts::getFont(FontStyle type, float scale)
{
    return this->getOrCreateFontData(type, scale).font;
}

QFontMetricsF Fonts::getFontMetrics(FontStyle type, float scale)
{
    return this->getOrCreateFontData(type, scale).metrics;
}

Fonts::FontData &Fonts::getOrCreateFontData(FontStyle type, float scale)
{
    assertInGuiThread();

    assert(type < FontStyle::EndType);

    auto &map = this->fontsByType_[size_t(type)];

    auto it = map.find(scale);
    if (it != map.end())
    {
        return it->second;
    }

    auto result = map.emplace(scale, Fonts::createFontData(type, scale));
    assert(result.second);

    return result.first->second;
}

Fonts::FontData Fonts::createFontData(FontStyle type, float scale)
{
    QFont font{
        fontFamily(type),
        static_cast<int>(fontSize(type) * scale),
        fontWeight(type),
        isItalic(type),
    };

    font.setStrikeOut(type == FontStyle::ChatMediumStrikethrough);

    switch (type)
    {
        case FontStyle::TimestampMedium: {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

            auto tag = QFont::Tag("tnum");
            font.setFeature(tag, 1);
#endif
        }
        break;

        default:
            break;
    }

    return font;
}

}  // namespace chatterino
