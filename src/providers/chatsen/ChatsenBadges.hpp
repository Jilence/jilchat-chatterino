// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHashFunctions>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class ChatsenBadges final
{
public:
    ChatsenBadges();

    // Retorna o badge de maior prioridade do usuário, se houver.
    // Prioridade: developer > early_supporter > early_bird > patreon tiers
    std::optional<EmotePtr> getBadge(const QString &userID) const;

    /// Loads the badges, replacing the ones loaded before.
    ///
    /// @param onDone called once the answer is there, with whether loading
    ///               worked; from any thread
    void load(const std::function<void(bool)> &onDone = {});

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, EmotePtr> badgeMap_;
};

}  // namespace chatterino
