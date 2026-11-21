// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/types/YourUsernameHighlight.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep
#include "singletons/Settings.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace chatterino::highlights {

namespace {

/// Whether `match` lies inside a mention of `domain` (or a subdomain of it) in
/// `message`
bool isMatchInsideDomain(const QRegularExpressionMatch &match,
                         const QString &message, const QString &domain)
{
    auto host = domain.trimmed().toLower();
    static const QRegularExpression scheme(R"(^https?://)");
    host.remove(scheme);
    host =
        host.section('/', 0, 0).section('?', 0, 0).section('#', 0, 0).section(
            ':', 0, 0);

    if (host.isEmpty())
    {
        return false;
    }

    const auto domainRegex = QRegularExpression(
        QStringLiteral(
            R"((?i)(?<![A-Z0-9.-])(?:[A-Z0-9-]+\.)*%1(?![A-Z0-9.-]))")
            .arg(QRegularExpression::escape(host)));

    auto it = domainRegex.globalMatch(message);
    while (it.hasNext())
    {
        const auto domainMatch = it.next();
        const auto domainStart = domainMatch.capturedStart();
        const auto domainEnd = domainStart + domainMatch.capturedLength();

        if (match.capturedStart() >= domainStart &&
            match.capturedStart() + match.capturedLength() <= domainEnd)
        {
            return true;
        }
    }

    return false;
}

/// Whether `re` matches `message` somewhere outside `domain`; with an empty
/// domain, whether it matches at all
bool matchesOutsideDomain(const QRegularExpression &re, const QString &message,
                          const QString &domain)
{
    auto it = re.globalMatch(message);
    if (domain.trimmed().isEmpty())
    {
        return it.hasNext();
    }
    while (it.hasNext())
    {
        if (!isMatchInsideDomain(it.next(), message, domain))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

HighlightCheck YourUsernameHighlight::buildCheck() const
{
    using H = std::remove_pointer_t<decltype(this)>;
    using Params = HighlightCheckParams;

    auto twitchRe = [&] -> std::optional<QRegularExpression> {
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        auto name = user->getUserName();
        if (user->isAnon() || name.isEmpty())
        {
            return std::nullopt;
        }

        return QRegularExpression{
            REGEX_START_BOUNDARY % QRegularExpression::escape(name) %
                REGEX_END_BOUNDARY,
            QRegularExpression::UseUnicodePropertiesOption |
                QRegularExpression::CaseInsensitiveOption,
        };
    }();
    auto kickRe = [&] -> std::optional<QRegularExpression> {
        auto user = getApp()->getAccounts()->kick.current();
        auto name = user->username();
        if (user->isAnonymous() || name.isEmpty())
        {
            return std::nullopt;
        }

        return QRegularExpression{
            REGEX_START_BOUNDARY % QRegularExpression::escape(name) %
                REGEX_END_BOUNDARY,
            QRegularExpression::UseUnicodePropertiesOption |
                QRegularExpression::CaseInsensitiveOption,
        };
    }();

    if (!twitchRe && !kickRe)
    {
        return {};
    }

    // Read once here; the checks are rebuilt when these change
    const auto domain = getSettings()->blockDomainHighlights
                            ? getSettings()->yourDomain.getValue()
                            : QString{};

    return {
        [highlight = *this, twitchRe, kickRe,
         domain](const Params &p) -> std::optional<HighlightResult> {
            if (p.self)
            {
                return std::nullopt;
            }

            switch (p.platform)
            {
                case MessagePlatform::AnyOrTwitch: {
                    if (!twitchRe || !matchesOutsideDomain(
                                         *twitchRe, p.originalMessage, domain))
                    {
                        return std::nullopt;
                    }
                }
                break;
                case MessagePlatform::Kick: {
                    if (!kickRe || !kickRe->match(p.originalMessage).hasMatch())
                    {
                        return std::nullopt;
                    }
                }
                break;
                // no youtube or tiktok account, match either twitch or kick
                // name
                case MessagePlatform::YouTube:
                case MessagePlatform::TikTok: {
                    bool matched =
                        (twitchRe &&
                         twitchRe->match(p.originalMessage).hasMatch()) ||
                        (kickRe && kickRe->match(p.originalMessage).hasMatch());
                    if (!matched)
                    {
                        return std::nullopt;
                    }
                }
                break;
            }

            return highlight.outcome.makeSimpleResult<H>();
        },
    };
}

QDebug operator<<(QDebug dbg, const YourUsernameHighlight &v)
{
    dbg.nospace() << "YourUsernameHighlight("        //
                  << "name:" << v.name               //
                  << ',' << "enabled:" << v.enabled  //
                  << ',' << "outcome:" << v.outcome  //
                  << ')';

    return dbg;
}

}  // namespace chatterino::highlights
