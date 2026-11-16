// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/EmoteLinkPreview.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Image.hpp"
#include "providers/links/LinkInfo.hpp"

#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <array>
#include <cstdint>
#include <optional>
#include <ranges>

namespace chatterino {

namespace {

enum class Provider : std::uint8_t { Bttv, Jiltv };

struct EmotePage {
    Provider provider;
    QString id;
    /// Newer BTTV links carry the name after the ID
    QString nameInLink;
};

}  // namespace

QString bttvEmoteIdFromLink(const QString &code)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{24}$"));
    if (hex.match(code).hasMatch())
    {
        return code;
    }
    // Base58 (as Bitcoin writes it) of the 12 bytes of the ID
    static const QString alphabet = QStringLiteral(
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz");
    std::array<quint8, 12> bytes{};
    for (const auto c : code)
    {
        const auto digit = alphabet.indexOf(c);
        if (digit < 0)
        {
            return {};
        }
        // bytes = bytes * 58 + digit
        auto carry = static_cast<quint32>(digit);
        for (auto &byte : std::ranges::reverse_view(bytes))
        {
            carry += static_cast<quint32>(byte) * 58;
            byte = static_cast<quint8>(carry & 0xFF);
            carry >>= 8;
        }
        if (carry != 0)
        {
            return {};  // longer than an ID
        }
    }
    QString id;
    for (const auto byte : bytes)
    {
        id += QStringLiteral("%1").arg(byte, 2, 16, u'0');
    }
    return id;
}

namespace {

/// Only the emote links the general link previews can't show: JilTV (its
/// pages have no preview of their own) and BTTV (whose emotes the link
/// previews can't look up anymore); 7TV and FFZ they show
std::optional<EmotePage> parse(const QString &url)
{
    static const QRegularExpression bttv(QStringLiteral(
        R"(betterttv\.com/emotes/([A-Za-z0-9]+)(?:/([^/?#]+))?)"));
    static const QRegularExpression jiltv(
        QStringLiteral(R"(jiltv\.app/emotes/([A-Za-z0-9_-]+))"));

    if (const auto m = bttv.match(url); m.hasMatch())
    {
        const auto id = bttvEmoteIdFromLink(m.captured(1));
        if (!id.isEmpty())
        {
            return EmotePage{
                .provider = Provider::Bttv,
                .id = id,
                .nameInLink = QUrl::fromPercentEncoding(m.captured(2).toUtf8()),
            };
        }
    }
    if (const auto m = jiltv.match(url); m.hasMatch())
    {
        return EmotePage{
            .provider = Provider::Jiltv,
            .id = m.captured(1),
            .nameInLink = {},
        };
    }
    return std::nullopt;
}

QString tooltipFor(const QString &name, const QString &provider,
                   const QString &owner)
{
    QString tooltip;
    if (!name.isEmpty())
    {
        tooltip += QStringLiteral("<b>") + name.toHtmlEscaped() +
                   QStringLiteral("</b><br>");
    }
    tooltip += provider + QStringLiteral(" Emote");
    if (!owner.isEmpty())
    {
        tooltip += QStringLiteral("<br>By: ") + owner.toHtmlEscaped();
    }
    return tooltip;
}

void finish(LinkInfo *info, const QString &picture, const QString &tooltip)
{
    if (!picture.isEmpty())
    {
        info->setThumbnail(Image::fromUrl(Url{picture}));
    }
    info->setTooltip(tooltip);
    info->setState(LinkInfo::State::Resolved);
}

}  // namespace

bool isEmotePageLink(const QString &url)
{
    return parse(url).has_value();
}

void resolveEmotePageLink(LinkInfo *info)
{
    const auto page = parse(info->originalUrl());
    if (!page)
    {
        return;
    }
    const auto id = page->id;

    switch (page->provider)
    {
        case Provider::Bttv:
            // BTTV only tells the name to signed-in users; the picture is
            // public
            finish(
                info,
                QStringLiteral("https://cdn.betterttv.net/emote/%1/3x").arg(id),
                tooltipFor(page->nameInLink, QStringLiteral("BTTV"), {}));
            return;
        case Provider::Jiltv: {
            const QJsonObject body{
                {"query",
                 QStringLiteral("query EmoteLink($id: ID!) { emote(id: $id) "
                                "{ name host owner { displayName } } }")},
                {"variables", QJsonObject{{"id", id}}},
            };
            NetworkRequest(QStringLiteral("https://api.jiltv.app/v1/graphql"),
                           NetworkRequestType::Post)
                .caller(info)
                .json(body)
                .timeout(15000)
                .onSuccess([info](const NetworkResult &result) {
                    const auto emote = result.parseJson()
                                           .value("data")
                                           .toObject()
                                           .value("emote")
                                           .toObject();
                    const auto host = emote.value("host").toString();
                    finish(info,
                           host.isEmpty() ? QString()
                                          : host + QStringLiteral("/4x.webp"),
                           tooltipFor(emote.value("name").toString(),
                                      QStringLiteral("JilTV"),
                                      emote.value("owner")
                                          .toObject()
                                          .value("displayName")
                                          .toString()));
                })
                .onError([info](const NetworkResult &) {
                    finish(info, {},
                           tooltipFor({}, QStringLiteral("JilTV"), {}));
                })
                .execute();
            return;
        }
    }
}

}  // namespace chatterino
