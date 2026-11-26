// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHashFunctions>
#include <QString>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// Enhancer's global badges (developer, founder, supporter milestones, ...),
/// from its public API. Channel badges aren't loaded.
class EnhancerBadges final
{
public:
    EnhancerBadges();

    /// The global Enhancer badges a Twitch user shows in chat, in Enhancer's
    /// order
    std::vector<EmotePtr> getBadges(const QString &userID) const;

    /// Loads the badges and who shows them, replacing the ones loaded before.
    ///
    /// @param onDone called once it's done, with whether loading worked
    void load(const std::function<void(bool)> &onDone = {});

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, std::vector<EmotePtr>> badgeMap_;
    bool loading_ = false;
};

}  // namespace chatterino
