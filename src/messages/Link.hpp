// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct Link {
public:
    enum Type {
        None,
        Url,
        UserInfo,
        UserWhisper,
        InsertText,
        UserAction,
        AutoModAllow,
        AutoModDeny,
        OpenAccountsPage,
        JumpToChannel,
        JumpToOrCreateChannel,
        Reconnect,
        CopyToClipboard,
        ReplyToMessage,
        ViewThread,
        JumpToMessage,
        AcknowledgeChatWarning,
        JilVoiceMessage,
        /// The value of these is the key of the AutoMod review
        AutoModReviewApprove,
        AutoModReviewDeny,
        /// The key alone asks for the duration, "<key>\n<seconds>" names it
        AutoModReviewTimeout,
        AutoModReviewBan,
        AutoModReviewRetry,
    };

    Link();
    Link(Type getType, const QString &getValue);

    Type type;
    QString value;

    bool isValid() const;
    bool isUrl() const;
};

}  // namespace chatterino
