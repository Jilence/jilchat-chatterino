// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

class LinkInfo;

/// Whether `url` is the page of an emote the general link previews can't
/// show (JilTV and BTTV). Such links get the emote as their
/// preview from its provider instead, like other links within the link
/// previews.
bool isEmotePageLink(const QString &url);

/// The BTTV emote ID (24 hex digits) a betterttv.com/emotes/... link points
/// to. Newer links carry it as a short Base58 code ("2nimqqQ9xgrwwDke6");
/// empty when `code` is neither.
QString bttvEmoteIdFromLink(const QString &code);

/// Fills `info` with the emote's picture, name and provider. Only for links
/// isEmotePageLink accepts.
void resolveEmotePageLink(LinkInfo *info);

}  // namespace chatterino
