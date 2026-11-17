// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ChannelNamePaint.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/bttv/BttvUsernameEffects.hpp"
#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"

#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPaintDevice>
#include <QPainter>

namespace chatterino {

namespace {

/// A channel's 7TV paint as looked up by its ID
struct SevenTvLookup {
    bool requested = false;
    /// Empty while unknown or without a paint
    QString paintId;
};

QHash<QString, SevenTvLookup> &sevenTvLookups()
{
    static QHash<QString, SevenTvLookup> lookups;
    return lookups;
}

/// Asks 7TV which paint the channel's broadcaster wears, and for the paint
/// itself when it isn't known yet.
void lookUpSevenTvPaint(const QString &login, const QString &roomId)
{
    NetworkRequest(QStringLiteral("https://7tv.io/v3/users/twitch/") + roomId)
        .timeout(15000)
        .onSuccess([login](const NetworkResult &result) {
            const auto paintId = result.parseJson()
                                     .value("user")
                                     .toObject()
                                     .value("style")
                                     .toObject()
                                     .value("paint_id")
                                     .toString();
            if (paintId.isEmpty())
            {
                return;
            }
            sevenTvLookups()[login].paintId = paintId;
            if (getApp()->getSeventvPaints()->getPaintById(paintId))
            {
                channelNamePaintsChanged().invoke();
                return;
            }
            static const auto query = QStringLiteral(
                "query($list: [ObjectID!]) { cosmetics(list: $list) { paints "
                "{ id name function color angle shape image_url repeat "
                "stops { at color } shadows { x_offset y_offset radius color "
                "} } } }");
            NetworkRequest(QStringLiteral("https://7tv.io/v3/gql"),
                           NetworkRequestType::Post)
                .json(QJsonObject{
                    {"query", query},
                    {"variables", QJsonObject{{"list", QJsonArray{paintId}}}},
                })
                .timeout(15000)
                .onSuccess([](const NetworkResult &result) {
                    const auto paints = result.parseJson()
                                            .value("data")
                                            .toObject()
                                            .value("cosmetics")
                                            .toObject()
                                            .value("paints")
                                            .toArray();
                    for (const auto &paint : paints)
                    {
                        getApp()->getSeventvPaints()->addPaint(
                            paint.toObject());
                    }
                    channelNamePaintsChanged().invoke();
                })
                .execute();
        })
        .execute();
}

/// Twitch name colors of broadcasters, by login; requested once each
struct ColorLookup {
    bool requested = false;
    QColor color;
};

QHash<QString, ColorLookup> &colorLookups()
{
    static QHash<QString, ColorLookup> lookups;
    return lookups;
}

/// Asks Twitch for the name color of the broadcaster with `roomId`
void lookUpColor(const QString &login, const QString &roomId)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon())
    {
        return;
    }
    NetworkRequest(
        QStringLiteral("https://api.twitch.tv/helix/chat/color?user_id=") +
        roomId)
        .header("Client-Id", account->getOAuthClient().toUtf8())
        .header("Authorization", "Bearer " + account->getOAuthToken().toUtf8())
        .timeout(15000)
        .onSuccess([login](const NetworkResult &result) {
            const auto data = result.parseJson().value("data").toArray();
            const QColor color(
                data.isEmpty()
                    ? QString()
                    : data.at(0).toObject().value("color").toString());
            if (color.isValid())
            {
                colorLookups()[login].color = color;
                channelNamePaintsChanged().invoke();
            }
        })
        .execute();
}

}  // namespace

QColor channelNameColor(TwitchChannel &channel)
{
    if (!getSettings()->channelNamePaints)
    {
        return {};
    }
    const auto login = channel.getName().toLower();
    // Seen in the chat: the color they write with now
    const auto seen = channel.getUserColor(login);
    if (seen.isValid())
    {
        return seen;
    }
    auto &lookup = colorLookups()[login];
    if (!lookup.requested && !channel.roomId().isEmpty())
    {
        lookup.requested = true;
        lookUpColor(login, channel.roomId());
    }
    return lookup.color;
}

pajlada::Signals::NoArgSignal &channelNamePaintsChanged()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

std::shared_ptr<Paint> channelNamePaint(const QString &login,
                                        const QString &roomId)
{
    if (!getSettings()->channelNamePaints || !usernamePaintsEnabled() ||
        login.isEmpty())
    {
        return nullptr;
    }
    const auto name = login.toLower();
    if (auto paint = usernamePaint(name, false))
    {
        return paint;
    }

    // A broadcaster who hasn't chatted isn't known to 7TV's live updates
    const auto source = getSettings()->usernamePaintSource.getEnum();
    if (source != UsernamePaintSource::SevenTV &&
        source != UsernamePaintSource::Automatic)
    {
        return nullptr;
    }
    auto &lookup = sevenTvLookups()[name];
    if (!lookup.requested && !roomId.isEmpty())
    {
        lookup.requested = true;
        lookUpSevenTvPaint(name, roomId);
    }
    if (lookup.paintId.isEmpty() ||
        getApp()->getSeventvPaints()->isPaintHidden(lookup.paintId))
    {
        return nullptr;
    }
    return getApp()->getSeventvPaints()->getPaintById(lookup.paintId);
}

void drawPaintedName(QPainter &painter, const QRectF &rect, const QString &text,
                     const QFont &font, QColor color, const Paint &paint,
                     Qt::Alignment alignment, float scale)
{
    const QFontMetricsF metrics(font);
    const QSizeF textSize(metrics.horizontalAdvance(text), metrics.height());
    QPointF topLeft(rect.left(), rect.top());
    if (alignment.testFlag(Qt::AlignHCenter))
    {
        topLeft.setX(rect.left() + ((rect.width() - textSize.width()) / 2));
    }
    else if (alignment.testFlag(Qt::AlignRight))
    {
        topLeft.setX(rect.right() - textSize.width());
    }
    if (alignment.testFlag(Qt::AlignVCenter))
    {
        topLeft.setY(rect.top() + ((rect.height() - textSize.height()) / 2));
    }
    else if (alignment.testFlag(Qt::AlignBottom))
    {
        topLeft.setY(rect.bottom() - textSize.height());
    }

    const auto dpr =
        painter.device() != nullptr
            ? static_cast<float>(painter.device()->devicePixelRatioF())
            : 1.0F;
    const auto margins = paint.overflowMargins(scale);
    const auto pixmap = paint.getNametagPixmap(text, font, color, textSize,
                                               scale, dpr, margins);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Not wider than the room for the name, like the text it replaces
    painter.setClipRect(rect.adjusted(-margins.left(), -margins.top(),
                                      margins.right(), margins.bottom()),
                        Qt::IntersectClip);
    painter.drawPixmap(QRectF(topLeft, textSize) + margins, pixmap,
                       QRectF(pixmap.rect()));
    painter.restore();
}

}  // namespace chatterino
