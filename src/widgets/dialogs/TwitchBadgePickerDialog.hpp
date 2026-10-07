// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace chatterino {

class MessageView;
class SvgButton;
class Button;
class TwitchChannel;

struct EventBadge {
    QString id;
    QString name;
    QString imageUrl;
    QString streamDatabaseUrl;
    QDateTime startAt;
    QDateTime endAt;
    std::optional<bool> free;
};

class TwitchBadgePickerDialog : public DraggablePopup
{
public:
    TwitchBadgePickerDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class View {
        GlobalBadges,
        ChannelBadges,
        EventBadges,
        Color,
        Bttv,
        BttvEffects,
        Moltorino,
        JilChat,
        Bluzyrino,
        SevenTvBadges,
        SevenTvPaints,
    };

    struct SevenTvPaint {
        QString id;
        QString name;
    };

    /// A Moltorino supporter badge and whether the user owns it.
    struct MoltorinoBadgeOption {
        GqlBadge badge;
        bool owned = false;
    };

    void loadBadges(bool force = false);
    void loadEventBadges(bool force = false);
    void switchView(View view);
    void rebuildContent();
    void rebuildGlobalBadges();
    void rebuildChannelBadges();
    void rebuildEventBadges();
    void rebuildColors();
    void loadMoltorinoBadges(bool force = false);
    void rebuildMoltorinoBadges();
    void selectMoltorino(const QString &badgeId);
    void loadJilChatBadges(bool force = false);
    void rebuildJilChatBadges();
    /// Makes the badge with `slug` the one shown in chat; none if empty.
    void selectJilChat(const QString &slug);
    void rebuildBluzyrinoBadges();
    /// Makes the donor badge `badgeId` the one shown in chat.
    void selectBluzyrino(const QString &badgeId, const QString &title);
    void loadBttv(bool force = false);
    void rebuildBttv();
    void connectBttv();
    void disconnectBttv();
    /// Shows the BetterTTV Pro badge `badgeId` (the latest one if empty) or,
    /// with `show` off, none.
    void selectBttv(bool show, const QString &badgeId);
    /// Styles the user's name with the BetterTTV effect `effect`; with none
    /// if empty.
    void selectBttvEffect(const QString &effect);
    /// Reads the username effects betterttv.com has now, once per run.
    void refreshBttvEffects();
    /// A request to BetterTTV as the connected user; a PATCH with `patch`.
    void bttvRequest(const QString &path,
                     const std::optional<QJsonObject> &patch,
                     const std::function<void(const QJsonObject &)> &onData,
                     const std::function<void(const QString &)> &onError);
    /// Asks for the token of the user's `service` account, which the
    /// `command` copies in the browser console on `site`.
    std::optional<QString> askForToken(const QString &service,
                                       const QString &command,
                                       const QString &site);
    void loadSevenTv(bool force = false);
    void rebuildSevenTv();
    void connectSevenTv();
    void disconnectSevenTv();
    void selectSevenTv(bool paint, const QString &id);
    void loadSevenTvPaintData();
    void sevenTvRequest(const QString &query, const QJsonObject &variables,
                        const std::function<void(const QJsonObject &)> &onData,
                        const std::function<void(const QString &)> &onError);
    void clearContent();
    void deselectChannel();
    void setFlairHidden(bool hidden);
    void refreshStyle();
    void setStatus(const QString &text, bool error = false);
    void selectGlobal(const GqlBadge &badge);
    void selectChannel(const GqlBadge &badge);
    void selectRole(const GqlBadge &badge);
    void selectColor(const QString &color);
    QString authTokenOrMessage();
    void applySizeConstraints();
    void updateSearchVisibility();
    void updatePreview();
    [[nodiscard]] MessagePtr buildPreviewMessage() const;
    void applyPreviewDialogWidth(int contentPixelWidth);
    [[nodiscard]] int badgeGridColumns() const;

    TwitchChannel *channel_{};

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QPushButton *globalTabButton_{};
    QPushButton *channelTabButton_{};
    QPushButton *eventTabButton_{};
    QPushButton *colorTabButton_{};
    QPushButton *bttvTabButton_{};
    QPushButton *bttvEffectTabButton_{};
    QPushButton *moltorinoTabButton_{};
    QPushButton *jilChatTabButton_{};
    QPushButton *bluzyrinoTabButton_{};
    QPushButton *sevenTvBadgeTabButton_{};
    QPushButton *sevenTvPaintTabButton_{};
    QWidget *searchRowWidget_{};
    QLineEdit *searchInput_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};
    QLabel *statusLabel_{};
    QWidget *previewWidget_{};
    MessageView *previewView_{};

    View view_ = View::GlobalBadges;
    GqlChatSettingsBadges badges_;
    bool badgesLoaded_ = false;
    bool badgesLoading_ = false;
    bool actionInFlight_ = false;
    bool initialFetchDone_ = false;
    QString statusText_;
    bool statusIsError_ = false;
    QString searchQuery_;
    int lastBadgeGridColumns_ = -1;

    QVector<EventBadge> eventBadgesCache_;
    bool eventBadgesLoading_ = false;
    bool eventBadgesLoaded_ = false;
    QDateTime eventBadgesCacheTime_;

    QVector<MoltorinoBadgeOption> moltorinoBadges_;
    /// The profile from /v2/badges/me, sent back with a new selection.
    QJsonObject moltorinoProfile_;
    QString moltorinoSelected_;
    bool moltorinoLoading_ = false;
    bool moltorinoLoaded_ = false;

    /// A JilChat badge and whether the user owns it. `badge.id` is its slug.
    struct JilChatBadgeOption {
        GqlBadge badge;
        /// Milestone badges are picked by their months instead, see the slug.
        QString badgeId;
        bool owned = false;
    };
    QVector<JilChatBadgeOption> jilChatBadges_;
    /// The slug of the badge shown in chat; empty for none.
    QString jilChatSelected_;
    bool jilChatLoading_ = false;
    bool jilChatLoaded_ = false;

    /// The Bluzyrino donor badge just picked, until the registry lists it.
    QString bluzyrinoPending_;

    /// The user's BetterTTV token, kept in the system credential store.
    QString bttvToken_;
    bool bttvTokenRead_ = false;
    /// The unlocked Pro badges, the latest one last.
    QVector<GqlBadge> bttvBadges_;
    bool bttvBadgeShown_ = false;
    /// The Pro badge picked; empty for always the latest one.
    QString bttvBadgeId_;
    QDateTime bttvNextUnlock_;
    /// The username effect in use; empty for none.
    QString bttvEffect_;
    /// The username effects by their id, true for the unlocked ones.
    QJsonObject bttvEffectEligibility_;
    bool bttvLoading_ = false;
    bool bttvLoaded_ = false;

    /// The user's 7TV token, kept in the system credential store.
    QString sevenTvToken_;
    bool sevenTvTokenRead_ = false;
    QString sevenTvUserId_;
    QVector<GqlBadge> sevenTvBadges_;
    QVector<SevenTvPaint> sevenTvPaints_;
    QString sevenTvActiveBadge_;
    QString sevenTvActivePaint_;
    bool sevenTvLoading_ = false;
    bool sevenTvLoaded_ = false;

    static std::vector<QPointer<TwitchBadgePickerDialog>> activeDialogs_;
};

}  // namespace chatterino
