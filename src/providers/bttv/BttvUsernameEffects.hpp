// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace chatterino {

class Paint;

/// A BetterTTV username effect as betterttv.com describes it. Most fill the
/// name with a moving image, `texture`, and outline it in `outline`.
struct BttvUsernameEffect {
    QString id;
    QString label;
    QString requirement;
    QString texture;
    QString outline;
};

/// The username effects of BetterTTV Pro and who uses which, to draw names
/// with them like with 7TV paints.
class BttvUsernameEffects
{
public:
    static BttvUsernameEffects &instance();

    /// The effects betterttv.com has, in its order; empty until `refresh`
    /// has read the page.
    QVector<BttvUsernameEffect> effects() const;

    /// The name for an effect of which only the id is known.
    static QString labelFromId(const QString &id);

    /// Reads the effects from betterttv.com, once per run. BetterTTV has no
    /// interface for them, so they are taken from the page's style sheet and
    /// script. Call it from the GUI thread.
    void refresh();

    /// Remembers that the user with the login `userName` uses `effect`;
    /// none if empty. Can be called from any thread.
    void setUserEffect(const QString &userName, const QString &effect);

    /// The paint that draws the effect of the user with the login
    /// `userName`, or nullptr. Call it from the GUI thread.
    std::shared_ptr<Paint> getPaint(const QString &userName) const;

    /// The effects were read from betterttv.com.
    pajlada::Signals::NoArgSignal effectsUpdated;

private:
    BttvUsernameEffects() = default;

    void applySite(const QString &style, const QString &script);

    std::atomic_bool refreshStarted_{false};

    mutable std::mutex mutex_;
    /// Guarded by mutex_.
    QVector<BttvUsernameEffect> effects_;
    /// Login -> effect id, guarded by mutex_.
    std::unordered_map<QString, QString> users_;
    /// Effect id -> its paint, made when first needed. Guarded by mutex_.
    mutable std::unordered_map<QString, std::shared_ptr<Paint>> paints_;
};

/// Whether names are drawn with any kind of paint.
bool usernamePaintsEnabled();

/// The paint for the name of the user with the login `userName`: the 7TV
/// paint or the BetterTTV username effect, whichever the user chose to see.
std::shared_ptr<Paint> usernamePaint(const QString &userName, bool kick);

}  // namespace chatterino
