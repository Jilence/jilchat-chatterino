// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvUsernameEffects.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Image.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"

#include <QBrush>
#include <QCache>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTransform>
#include <QUrl>

#include <algorithm>

namespace {

using namespace chatterino;

constexpr auto SITE_URL = "https://betterttv.com";
/// The images of the effects as the BetterTTV extension uses them in the web
/// chat, by the effect's id.
constexpr auto TEXTURE_URL =
    "https://cdn.betterttv.net/assets/username_effects/%1.png";
/// BetterTTV keeps telling the old effect for up to about a minute after a
/// change, so asking once isn't enough; it answers about every ten seconds.
constexpr qint64 ASK_AGAIN_AFTER_MS = 20'000;

/// Fills the name with an image that moves diagonally, once around its 96
/// pixels in 16 seconds, and outlines it.
///
/// It draws the name itself: the general way blurs a shadow for every
/// outline again in every frame of every name, which is too slow for a
/// paint that always moves.
class TexturePaint final : public Paint
{
public:
    TexturePaint(const BttvUsernameEffect &effect)
        : Paint(effect.id, effect.label)
        , image_(Image::fromUrl(Url{effect.texture}))
        , outline_(effect.outline)
    {
    }

    QPixmap getPixmap(const QString &text, const QFont &font, QColor userColor,
                      QSizeF size, float /*scale*/, float dpr,
                      bool centerVertically, qreal padding) const override
    {
        const auto texture = this->image_->pixmapOrLoad();
        const bool outlined = this->shadowsEnabled();

        // The image moves by a fraction of a pixel per frame. A new picture
        // every half pixel looks the same and is drawn far less often.
        const auto step = static_cast<int>(
            double(QDateTime::currentMSecsSinceEpoch() % 16000) * 96.0 * 2.0 /
            16000.0);
        const auto key =
            QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                .arg(text, font.key())
                .arg(size.width())
                .arg(size.height())
                .arg(dpr)
                .arg(centerVertically)
                .arg(padding)
                .arg(outlined)
                .arg(texture ? QString{} : userColor.name(QColor::HexArgb));
        if (const auto *frame = this->frames_.object(key);
            frame != nullptr && frame->step == step)
        {
            return frame->pixmap;
        }

        const QSizeF drawSize = size + QSizeF(2 * padding, 2 * padding);
        QPixmap pixmap((drawSize * dpr).toSize());
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);

        // The colon after the name isn't part of the name.
        auto nametag = text;
        const bool colon = nametag.endsWith(u':');
        if (colon)
        {
            nametag.chop(1);
        }

        const QRectF textRect(QPointF{padding, padding}, size);
        const QFontMetricsF metrics(font);
        const auto baseline =
            centerVertically
                ? textRect.top() +
                      ((textRect.height() - metrics.height()) / 2) +
                      metrics.ascent()
                : textRect.bottom() - metrics.descent();
        QPainterPath name;
        name.addText(textRect.left(), baseline, font, nametag);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (outlined)
        {
            // What the extension does in the web chat: the shape of the
            // name, grown by a pixel to all sides, in the outline's color;
            // and the same again two pixels lower. There, the name is in a
            // font of 13 pixels, whose capital letters are about 9.4 pixels
            // high; measured in a screenshot, the outline is a ninth of
            // that. So a "pixel" here goes by the height of the capitals.
            const auto unit = metrics.capHeight() / 9.4;
            painter.setPen(QPen(this->outline_, 2 * unit, Qt::SolidLine,
                                Qt::SquareCap, Qt::MiterJoin));
            painter.setBrush(this->outline_);
            // The next line is close below the name's. In the web chat the
            // lines are further apart; here, the outline ends with the line.
            painter.setClipRect(
                QRectF(0, 0, drawSize.width(), textRect.bottom()));
            painter.drawPath(name.translated(0, 2 * unit));
            painter.drawPath(name);
            painter.setClipping(false);
        }
        painter.setPen(Qt::NoPen);
        if (texture)
        {
            QBrush brush(*texture);
            brush.setTransform(
                QTransform::fromTranslate(-step / 2.0, -step / 2.0));
            painter.setBrush(brush);
        }
        else
        {
            painter.setBrush(userColor);
        }
        painter.drawPath(name);

        if (colon)
        {
            painter.setPen(getApp()->getThemes()->messages.textColors.regular);
            painter.setFont(font);
            painter.drawText(
                QPointF(textRect.left() + metrics.horizontalAdvance(nametag),
                        baseline),
                QStringLiteral(":"));
        }
        painter.end();

        this->frames_.insert(key, new Frame{.step = step, .pixmap = pixmap});
        return pixmap;
    }

    QBrush asBrush(QColor userColor, QRectF /*drawingRect*/) const override
    {
        // getPixmap doesn't ask for it.
        return {userColor};
    }

    const std::vector<PaintDropShadow> &getDropShadows() const override
    {
        // The outline is drawn with the name.
        static const std::vector<PaintDropShadow> none;
        return none;
    }

    bool animated() const override
    {
        return true;
    }

    bool shadowsEnabled() const override
    {
        return getSettings()->displayBttvUsernameEffectOutlines;
    }

    qreal overflow() const override
    {
        // The outline is around the name.
        return this->shadowsEnabled() ? 3 : 0;
    }

    QString sourceName() const override
    {
        return QStringLiteral("BTTV");
    }

private:
    /// A name as it was drawn last, to show again while the image hasn't
    /// moved on.
    struct Frame {
        int step;
        QPixmap pixmap;
    };

    ImagePtr image_;
    QColor outline_;
    mutable QCache<QString, Frame> frames_{128};
};

/// The name in its color; a light crosses it in the first half of every
/// eight seconds.
class FlarePaint final : public Paint
{
public:
    FlarePaint(const QString &id, const QString &label)
        : Paint(id, label)
    {
    }
    ~FlarePaint() override = default;

    QBrush asBrush(QColor userColor, QRectF drawingRect) const override
    {
        const auto phase =
            double(QDateTime::currentMSecsSinceEpoch() % 8000) / 4000.0;
        if (phase >= 1.0)
        {
            return {userColor};
        }
        const auto width = drawingRect.width();
        const auto center = drawingRect.left() - width + (4 * width * phase);
        const auto reach = 0.2 * width;
        const QColor shine((userColor.red() + (4 * 255)) / 5,
                           (userColor.green() + (4 * 255)) / 5,
                           (userColor.blue() + (4 * 255)) / 5);
        QLinearGradient gradient(center - reach, 0, center + reach, 0);
        gradient.setColorAt(0, userColor);
        gradient.setColorAt(0.5, shine);
        gradient.setColorAt(1, userColor);
        return gradient;
    }

    const std::vector<PaintDropShadow> &getDropShadows() const override
    {
        static const std::vector<PaintDropShadow> none;
        return none;
    }

    bool animated() const override
    {
        return true;
    }

    QString sourceName() const override
    {
        return QStringLiteral("BTTV");
    }

private:
    Q_DISABLE_COPY_MOVE(FlarePaint)
};

/// The name in its color with a light of that color around it.
class GlowPaint final : public Paint
{
public:
    GlowPaint(const QString &id, const QString &label)
        : Paint(id, label)
    {
    }
    ~GlowPaint() override = default;

    QBrush asBrush(QColor userColor, QRectF /*drawingRect*/) const override
    {
        // The light has the color of the name, which is only known here.
        auto light = userColor;
        light.setAlphaF(0.8F);
        this->dropShadows_.clear();
        this->dropShadows_.emplace_back(0.F, 0.F, 4.F, light, true);
        return {userColor};
    }

    const std::vector<PaintDropShadow> &getDropShadows() const override
    {
        return this->dropShadows_;
    }

    bool animated() const override
    {
        return false;
    }

    bool shadowsEnabled() const override
    {
        // The light is all there is to this effect.
        return true;
    }

    QString sourceName() const override
    {
        return QStringLiteral("BTTV");
    }

private:
    Q_DISABLE_COPY_MOVE(GlowPaint)

    mutable std::vector<PaintDropShadow> dropShadows_;
};

}  // namespace

namespace chatterino {

BttvUsernameEffects &BttvUsernameEffects::instance()
{
    static BttvUsernameEffects instance;
    return instance;
}

QVector<BttvUsernameEffect> BttvUsernameEffects::effects() const
{
    const std::scoped_lock lock(this->mutex_);
    return this->effects_;
}

QString BttvUsernameEffects::labelFromId(const QString &id)
{
    return id.isEmpty() ? id : id.at(0).toUpper() + id.mid(1);
}

void BttvUsernameEffects::refresh()
{
    if (this->refreshStarted_.exchange(true))
    {
        return;
    }

    // The page names its script and its style sheet; these have the effects.
    const auto site = QString::fromLatin1(SITE_URL);
    NetworkRequest(QUrl(site + QStringLiteral("/dashboard/pro")))
        .timeout(15000)
        .onSuccess([this, site](const NetworkResult &page) {
            static const QRegularExpression scriptRegex(
                R"re("(/assets/index-[^"]+\.js)")re");
            static const QRegularExpression styleRegex(
                R"re("(/assets/index-[^"]+\.css)")re");
            const auto html = QString::fromUtf8(page.getData());
            const auto script = scriptRegex.match(html).captured(1);
            const auto style = styleRegex.match(html).captured(1);
            if (script.isEmpty() || style.isEmpty())
            {
                return;
            }
            NetworkRequest(QUrl(site + style))
                .timeout(15000)
                .onSuccess([this, site, script](const NetworkResult &css) {
                    const auto styleText = QString::fromUtf8(css.getData());
                    NetworkRequest(QUrl(site + script))
                        .timeout(30000)
                        .onSuccess([this, styleText](const NetworkResult &js) {
                            this->applySite(styleText,
                                            QString::fromUtf8(js.getData()));
                        })
                        .execute();
                })
                .execute();
        })
        .execute();
}

void BttvUsernameEffects::applySite(const QString &style, const QString &script)
{
    static const QRegularExpression textureRegex(
        R"re(\._([a-z]+)_\w+\{[^}]*background-image:url\((/assets/[^)]+\.png)\))re");
    static const QRegularExpression outlineRegex(
        R"re("stroke-text-svg-filter-([a-z]+)":`(#[0-9a-fA-F]{3,8})`)re");
    static const QRegularExpression labelRegex(
        R"re(label:\w+\.formatMessage\(\{id:`[^`]*`,defaultMessage:\[\{type:0,value:`([^`]*)`\}\]\}\),requirement:\w+\.formatMessage\(\{id:`[^`]*`,defaultMessage:\[\{type:0,value:`([^`]*)`\}\]\}\))re");

    QHash<QString, QString> textures;
    for (auto it = textureRegex.globalMatch(style); it.hasNext();)
    {
        const auto match = it.next();
        textures.insert(match.captured(1),
                        QString::fromLatin1(SITE_URL) + match.captured(2));
    }
    QHash<QString, QString> outlines;
    QStringList outlineOrder;
    for (auto it = outlineRegex.globalMatch(script); it.hasNext();)
    {
        const auto match = it.next();
        outlines.insert(match.captured(1), match.captured(2));
        outlineOrder.push_back(match.captured(1));
    }

    QVector<BttvUsernameEffect> updated;
    QSet<QString> listed;
    for (auto it = labelRegex.globalMatch(script); it.hasNext();)
    {
        const auto match = it.next();
        const auto id = match.captured(1).toLower();
        if (id.isEmpty() || listed.contains(id))
        {
            continue;
        }
        updated.push_back({
            .id = id,
            .label = match.captured(1),
            .requirement = match.captured(2),
            .texture = {},
            .outline = {},
        });
        listed.insert(id);
    }
    // An effect with an image the page describes in a way not known here.
    for (const auto &id : std::as_const(outlineOrder))
    {
        if (!listed.contains(id) && textures.contains(id))
        {
            updated.push_back({
                .id = id,
                .label = labelFromId(id),
                .requirement = {},
                .texture = {},
                .outline = {},
            });
            listed.insert(id);
        }
    }
    for (auto &effect : updated)
    {
        if (textures.contains(effect.id) && outlines.contains(effect.id))
        {
            effect.texture = QString::fromLatin1(TEXTURE_URL).arg(effect.id);
            effect.outline = outlines.value(effect.id);
        }
    }

    {
        const std::scoped_lock lock(this->mutex_);
        this->effects_ = updated;
        this->paints_.clear();
    }
    this->effectsUpdated.invoke();
    if (auto *app = tryGetApp())
    {
        app->getWindows()->invalidateChannelViewBuffers();
    }
}

void BttvUsernameEffects::setUserEffect(const QString &userName,
                                        const QString &effect)
{
    const auto login = userName.toLower();
    if (login.isEmpty())
    {
        return;
    }

    {
        const std::scoped_lock lock(this->mutex_);
        this->known_[login] =
            QDateTime::currentMSecsSinceEpoch() + ASK_AGAIN_AFTER_MS;

        const auto it = this->users_.find(login);
        if (effect.isEmpty())
        {
            if (it == this->users_.end())
            {
                return;
            }
            this->users_.erase(it);
        }
        else
        {
            if (it != this->users_.end() && it->second == effect)
            {
                return;
            }
            this->users_[login] = effect;
        }
    }

    runInGuiThread([this] {
        this->refresh();
        if (auto *app = tryGetApp())
        {
            app->getWindows()->invalidateChannelViewBuffers();
        }
    });
}

void BttvUsernameEffects::userActive(const QString &userName,
                                     const QString &userId,
                                     const QString &channelId)
{
    if (userId.isEmpty() || channelId.isEmpty())
    {
        return;
    }

    {
        const std::scoped_lock lock(this->mutex_);
        const auto it = this->known_.find(userName.toLower());
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (it == this->known_.end() || now < it->second)
        {
            return;
        }
        it->second = now + ASK_AGAIN_AFTER_MS;
    }

    // BetterTTV answers an announcement with what it knows about the user.
    auto *app = tryGetApp();
    if (app != nullptr && app->getBttvLiveUpdates() != nullptr)
    {
        app->getBttvLiveUpdates()->broadcastMe(channelId, userId);
    }
}

std::shared_ptr<Paint> BttvUsernameEffects::getPaint(
    const QString &userName) const
{
    const std::scoped_lock lock(this->mutex_);
    const auto user = this->users_.find(userName);
    if (user == this->users_.end())
    {
        return nullptr;
    }
    const auto &id = user->second;
    if (const auto it = this->paints_.find(id); it != this->paints_.end())
    {
        return it->second;
    }

    auto label = labelFromId(id);
    std::shared_ptr<Paint> paint;
    for (const auto &effect : std::as_const(this->effects_))
    {
        if (effect.id != id)
        {
            continue;
        }
        label = effect.label;
        if (!effect.texture.isEmpty())
        {
            paint = std::make_shared<TexturePaint>(effect);
        }
    }
    if (paint == nullptr && id == u"flare")
    {
        paint = std::make_shared<FlarePaint>(id, label);
    }
    if (paint == nullptr && id == u"glow")
    {
        paint = std::make_shared<GlowPaint>(id, label);
    }
    if (paint != nullptr)
    {
        this->paints_[id] = paint;
    }
    return paint;
}

bool usernamePaintsEnabled()
{
    return getSettings()->usernamePaintSource.getEnum() !=
           UsernamePaintSource::Off;
}

std::shared_ptr<Paint> usernamePaint(const QString &userName, bool kick)
{
    const auto source = getSettings()->usernamePaintSource.getEnum();

    if (source == UsernamePaintSource::SevenTV ||
        source == UsernamePaintSource::Automatic)
    {
        auto paint = getApp()->getSeventvPaints()->getPaint(userName, kick);
        if (paint)
        {
            return paint;
        }
    }
    if (!kick && (source == UsernamePaintSource::BetterTTV ||
                  source == UsernamePaintSource::Automatic))
    {
        return BttvUsernameEffects::instance().getPaint(userName);
    }
    return nullptr;
}

}  // namespace chatterino
