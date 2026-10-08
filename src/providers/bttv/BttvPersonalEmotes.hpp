// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QJsonArray>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;

/// The personal emotes of BetterTTV Pro users: emotes a user can use in any
/// channel. BetterTTV tells about them together with the user's badge.
class BttvPersonalEmotes
{
public:
    static BttvPersonalEmotes &instance();

    /// Remembers the personal emotes of the Twitch user with the id
    /// `userId`, as BetterTTV lists them; none if `emotes` is empty. Can be
    /// called from any thread.
    void setUserEmotes(const QString &userId, const QJsonArray &emotes);

    /// The personal emote `name` of the Twitch user with the id `userId`,
    /// if they have it and personal emotes are turned on.
    std::optional<EmotePtr> getEmote(const QString &userId,
                                     EmoteNameView name) const;

    /// All personal emotes of the Twitch user with the id `userId`, or
    /// nullptr; also if personal emotes are turned off.
    std::shared_ptr<const EmoteMap> getEmotes(const QString &userId) const;

private:
    BttvPersonalEmotes() = default;

    mutable std::mutex mutex_;
    /// Twitch user id -> what BetterTTV listed last, to skip what didn't
    /// change. Guarded by mutex_.
    std::unordered_map<QString, QJsonArray> listed_;
    /// Twitch user id -> the user's emotes. Guarded by mutex_.
    std::unordered_map<QString, std::shared_ptr<const EmoteMap>> emotes_;
};

}  // namespace chatterino
