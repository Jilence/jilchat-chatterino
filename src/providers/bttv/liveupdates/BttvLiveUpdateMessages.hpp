// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace chatterino {

struct BttvLiveUpdateEmoteUpdateAddMessage {
    BttvLiveUpdateEmoteUpdateAddMessage(const QJsonObject &json);

    QString channelID;

    QJsonObject jsonEmote;
    QString emoteName;
    QString emoteID;

    bool validate() const;

private:
    bool badChannelID_;
};

struct BttvLiveUpdateEmoteRemoveMessage {
    BttvLiveUpdateEmoteRemoveMessage(const QJsonObject &json);

    QString channelID;
    QString emoteID;

    bool validate() const;

private:
    bool badChannelID_;
};

struct BttvLiveUpdateUserUpdateMessage {
    BttvLiveUpdateUserUpdateMessage(const QJsonObject &json);

    QString userID;
    QString userName;
    /// The id of the user's username effect; empty for none.
    QString usernameEffect;
    /// The user's personal emotes.
    QJsonArray emotes;
    QJsonObject badgeObject;

    bool validate() const;
    bool hasBadge() const;
};

}  // namespace chatterino
