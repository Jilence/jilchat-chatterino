// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/highlights/HighlightChannelScope.hpp"
#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep

#include <QString>
#include <rapidjson/document.h>

#include <vector>

namespace chatterino::highlights {

/// What a highlight can have beyond Chatterino's own fields: how the matched
/// part of the message is marked, and the channels it applies in. Stored
/// with the highlight under the same keys the old highlight phrases used.
struct HighlightExtras {
    /// How the matched text is marked, e.g. "Outline". Empty: not set.
    QString matchStyle;
    /// The color of that marker as #AARRGGBB. Empty: not set.
    QString matchColor;
    /// Where the highlight applies, e.g. "all". Empty: everywhere.
    QString channelScope;
    /// The channels for `channelScope`, encoded like HighlightChannelScope
    /// does it
    std::vector<QString> channelTargets;

    bool operator==(const HighlightExtras &other) const = default;

    HighlightChannelScope getChannelScope() const
    {
        std::vector<HighlightChannelTarget> targets;
        for (const auto &encoded : this->channelTargets)
        {
            if (auto target = decodeHighlightChannelTarget(encoded))
            {
                targets.emplace_back(std::move(*target));
            }
        }
        return {highlightChannelScopeModeFromKey(this->channelScope),
                std::move(targets)};
    }

    void setChannelScope(const HighlightChannelScope &scope)
    {
        this->channelTargets.clear();
        if (scope.mode() == HighlightChannelScopeMode::Everywhere)
        {
            this->channelScope.clear();
            return;
        }
        this->channelScope = highlightChannelScopeModeKey(scope.mode());
        for (const auto &target : scope.targets())
        {
            this->channelTargets.emplace_back(
                encodeHighlightChannelTarget(target));
        }
    }

    void serialize(rapidjson::Value &ret,
                   rapidjson::Document::AllocatorType &a) const
    {
        if (!this->matchStyle.isEmpty())
        {
            rj::set(ret, "matchStyle", this->matchStyle, a);
        }
        if (!this->matchColor.isEmpty())
        {
            rj::set(ret, "matchColor", this->matchColor, a);
        }
        if (!this->channelScope.isEmpty())
        {
            rj::set(ret, "channelScope", this->channelScope, a);
        }
        if (!this->channelTargets.empty())
        {
            rj::set(ret, "channelTargets", this->channelTargets, a);
        }
    }

    void deserialize(const rapidjson::Value &value)
    {
        rj::getSafe(value, "matchStyle", this->matchStyle);
        rj::getSafe(value, "matchColor", this->matchColor);
        rj::getSafe(value, "channelScope", this->channelScope);
        rj::getSafe(value, "channelTargets", this->channelTargets);
    }
};

}  // namespace chatterino::highlights
