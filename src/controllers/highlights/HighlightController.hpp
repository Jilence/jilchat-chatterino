// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "common/UniqueAccess.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "singletons/Settings.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/settings.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QStringView>
#include <QUrl>

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chatterino {

class TwitchBadge;
struct MessageParseArgs;
class AccountController;
enum class MessageFlag : std::int64_t;
using MessageFlags = FlagsEnum<MessageFlag>;

namespace filters {

struct RunContext;

}  // namespace filters

class HighlightController final
{
public:
    HighlightController(Settings &settings, AccountController *accounts);

    [[nodiscard]] std::pair<bool, HighlightResult> check(
        const HighlightCheckParams &params) const;

    /// Returns a set of built-in highlight IDs (e.g. {"whispers", "yourusername"})
    static std::unordered_set<QStringView> billTinHighlights();

    /// Special case where we want to be able to check if the Subscribed Thread highlight is enabled
    /// Whenever highlights are rebuilt, this variable might change.
    bool isSubscribedThreadHighlightEnabled = false;

private:
    void rebuildChecks(Settings &settings);

    UniqueAccess<std::vector<HighlightCheck>> checks_;

    pajlada::Signals::SignalHolder signalHolder_;
    std::vector<boost::signals2::scoped_connection> bConnections;
};

}  // namespace chatterino
