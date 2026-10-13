// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/MessageBuilder.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "util/Helpers.hpp"

#include <QStringBuilder>

#include <algorithm>
#include <limits>

namespace {

using namespace chatterino;
using namespace chatterino::eventsub;
using namespace chatterino::literals;

/// <MODERATOR> turned {on/off} <MODE> mode. [<DURATION>]
void makeModeMessage(EventSubMessageBuilder &builder,
                     const lib::payload::channel_moderate::v2::Event &event,
                     const QString &mode, bool on, const QString &duration = {})
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"turned"_s, text);
    QString op = on ? u"on"_s : u"off"_s;
    builder.emplaceSystemTextAndUpdate(op, text);
    builder.emplaceSystemTextAndUpdate(mode, text);
    builder.emplaceSystemTextAndUpdate(u"mode."_s, text);

    if (!duration.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(duration, text);
    }

    builder.setMessageAndSearchText(text);
}

// XXX: this is a duplicate from messages/MessageBuilder.cpp
EmotePtr makeAutoModBadge()
{
    return std::make_shared<Emote>(Emote{
        .name = EmoteName{},
        .images =
            ImageSet{Image::fromResourcePixmap(getResources().twitch.automod)},
        .tooltip = Tooltip{"AutoMod"},
        .homePage =
            Url{"https://dashboard.twitch.tv/settings/moderation/automod"},
    });
}

QString localizedDisplayName(const automod::ReviewItem &item)
{
    QString displayName =
        item.userName.isEmpty() ? item.userLogin : item.userName;
    const bool hasLocalizedName =
        displayName.compare(item.userLogin, Qt::CaseInsensitive) != 0;

    switch (getSettings()->usernameDisplayMode.getValue())
    {
        case UsernameDisplayMode::Username:
            return item.userLogin;
        case UsernameDisplayMode::UsernameAndLocalizedName:
            if (hasLocalizedName)
            {
                return item.userLogin % u'(' % displayName % u')';
            }
            break;
        case UsernameDisplayMode::LocalizedName:
        default:
            break;
    }
    return displayName;
}

/// Adds the text of the held message word by word, so that links and mentions
/// in it show up as such
void appendHeldMessageText(EventSubMessageBuilder &builder,
                           TwitchChannel *channel, const QString &message)
{
    for (const auto &word : message.split(u' '))
    {
        if (word.isEmpty())
        {
            continue;
        }
        builder.addWordFromUserMessage(word, channel);
    }
}

/// Everything but the held message itself (the elements from `heldBegin` up
/// to `heldEnd`) is no message text, so nothing is marked in it.
void ignoreMatchesOutsideHeldText(EventSubMessageBuilder &builder,
                                  size_t heldBegin, size_t heldEnd)
{
    auto &elements = builder->elements;
    for (size_t i = 0; i < elements.size(); ++i)
    {
        if (i < heldBegin || i >= heldEnd)
        {
            elements[i]->addFlags(MessageElementFlag::IgnoreExactMatch);
        }
    }
}

/// What AutoMod matched in the held message, to be marked in its text
std::shared_ptr<const std::vector<HighlightMatch>> makeAutoModMatches(
    const automod::ReviewItem &item)
{
    if (item.matchRanges.isEmpty())
    {
        return nullptr;
    }

    QString rule = u"Twitch AutoMod"_s;
    if (!item.reasonSummary.isEmpty())
    {
        rule += u'\n' % item.reasonSummary;
    }

    auto matches = std::make_shared<std::vector<HighlightMatch>>();
    matches->reserve(static_cast<size_t>(item.matchRanges.size()));
    for (const auto &range : item.matchRanges)
    {
        matches->push_back({
            .start = range.start,
            .length = range.length,
            .color = defaultAutoModMatchColor(),
            // a term another channel shares with this one
            .ruleName =
                range.termOwnerLogin.isEmpty() ||
                        range.termOwnerLogin.compare(item.broadcasterLogin,
                                                     Qt::CaseInsensitive) == 0
                    ? rule
                    : QString(rule % u"\nShared from #"_s %
                              range.termOwnerLogin),
            .pattern = item.messageText.mid(range.start, range.length),
            .source = HighlightMatchSource::AutoMod,
            .style = HighlightMatchStyle::Fill,
            .paintID = {},
        });
    }
    return matches;
}

/// Where the codepoint at `codepointOffset` begins in `text`, or -1
qsizetype utf16OffsetForCodepoint(QStringView text, int codepointOffset)
{
    if (codepointOffset < 0)
    {
        return -1;
    }

    qsizetype utf16 = 0;
    int codepoints = 0;
    while (utf16 < text.size() && codepoints < codepointOffset)
    {
        if (text.at(utf16).isHighSurrogate() && utf16 + 1 < text.size() &&
            text.at(utf16 + 1).isLowSurrogate())
        {
            utf16 += 2;
        }
        else
        {
            ++utf16;
        }
        ++codepoints;
    }
    return codepoints == codepointOffset ? utf16 : -1;
}

/// Remembers the part of the message Twitch points at with `boundary`
void appendMatchedFragment(automod::HoldData &data,
                           const lib::automod::Boundary &boundary,
                           const QString &termOwnerLogin = {})
{
    if (boundary.endPos < boundary.startPos ||
        boundary.endPos == std::numeric_limits<int>::max())
    {
        return;
    }

    // Twitch counts in codepoints
    const auto start =
        utf16OffsetForCodepoint(data.messageText, boundary.startPos);
    const auto end =
        utf16OffsetForCodepoint(data.messageText, boundary.endPos + 1);
    if (start < 0 || end <= start)
    {
        return;
    }

    const auto fragment = data.messageText.mid(start, end - start);
    if (!data.matchedFragments.contains(fragment, Qt::CaseInsensitive))
    {
        data.matchedFragments.push_back(fragment);
    }
    data.matchRanges.push_back({
        .start = start,
        .length = end - start,
        .termOwnerLogin = termOwnerLogin,
    });
}

/// Sorts the matched ranges and joins those that overlap or only have
/// whitespace between them
void mergeMatchRanges(automod::HoldData &data)
{
    if (data.matchRanges.size() < 2)
    {
        return;
    }

    std::ranges::sort(data.matchRanges, {}, &automod::MatchRange::start);
    QVector<automod::MatchRange> merged;
    merged.reserve(data.matchRanges.size());
    for (const auto &range : data.matchRanges)
    {
        if (!merged.empty())
        {
            auto &previous = merged.back();
            const auto previousEnd = previous.start + previous.length;
            const auto gap = range.start - previousEnd;
            // terms of different channels stay apart: they say so
            if (previous.termOwnerLogin == range.termOwnerLogin &&
                (gap <= 0 ||
                 data.messageText.mid(previousEnd, gap).trimmed().isEmpty()))
            {
                previous.length =
                    std::max(previousEnd, range.start + range.length) -
                    previous.start;
                continue;
            }
        }
        merged.push_back(range);
    }
    data.matchRanges = std::move(merged);
}

QString displayAutoModCategory(QString category)
{
    category.replace(u'_', u' ');
    if (!category.isEmpty())
    {
        category[0] = category.at(0).toUpper();
    }
    return category;
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::AutomodReason &reason)
{
    data.blockedTerm = false;
    data.reasonCategory = displayAutoModCategory(reason.category.qt());
    data.reasonLevel = reason.level;
    data.reasonSummary =
        data.reasonCategory % u" · Level "_s % QString::number(reason.level);
    for (const auto &boundary : reason.boundaries)
    {
        appendMatchedFragment(data, boundary);
    }
    mergeMatchRanges(data);
}

void fillAutoModReason(automod::HoldData &data, const std::string &reason)
{
    data.blockedTerm = false;
    data.reasonCategory =
        displayAutoModCategory(QString::fromStdString(reason));
    data.reasonSummary = data.reasonCategory;
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::BlockedTermReason &reason)
{
    data.blockedTerm = true;
    data.reasonCategory = u"Blocked term"_s;
    for (const auto &found : reason.termsFound)
    {
        appendMatchedFragment(
            data, found.boundary,
            QString::fromStdString(found.ownerBroadcasterUserLogin));
        if (data.termOwnerLogin.isEmpty())
        {
            data.termOwnerLogin =
                QString::fromStdString(found.ownerBroadcasterUserLogin);
        }
    }

    mergeMatchRanges(data);

    const auto count = data.matchedFragments.size();
    if (count == 0)
    {
        data.reasonSummary = u"Blocked term usage"_s;
        return;
    }

    data.reasonSummary = count == 1 ? u"Matched 1 blocked term"_s
                                    : u"Matched %1 blocked terms"_s.arg(count);
}

}  // namespace

namespace chatterino::eventsub {

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel,
                                               const QDateTime &time)
    : channel(channel)
{
    this->emplace<TimestampElement>(time.time());
    this->message().flags.set(MessageFlag::System, MessageFlag::EventSub);
    this->message().serverReceivedTime = time;
}

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel)
    : channel(channel)
{
    this->message().flags.set(MessageFlag::EventSub);
}

EventSubMessageBuilder::~EventSubMessageBuilder() = default;

void EventSubMessageBuilder::appendUser(const lib::String &userName,
                                        const lib::String &userLogin,
                                        QString &text, bool trailingSpace)
{
    auto login = userLogin.qt();
    auto *el = this->emplace<MentionElement>(
        userName.qt(), login, MessageColor::System,
        this->channel->getUserColor(login));
    text.append(login);

    if (trailingSpace)
    {
        text.append(u' ');
    }
    else
    {
        el->setTrailingSpace(false);
    }
}

void EventSubMessageBuilder::setMessageAndSearchText(const QString &text)
{
    assert(this->message().messageText.isNull());
    assert(this->message().searchText.isNull());

    this->message().messageText = text;
    this->message().searchText = text;
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Vip &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has added", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unvip &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has removed", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Warn &action)
{
    builder->flags.set(MessageFlag::ModerationAction);
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has warned", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    QStringList reasons;

    if (!action.reason.qt().isEmpty())
    {
        reasons.append(action.reason.qt());
    }

    for (const auto &rule : action.chatRulesCited)
    {
        if (!rule.qt().isEmpty())
        {
            reasons.append(rule.qt());
        }
    }

    if (reasons.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.emplaceSystemTextAndUpdate(reasons.join(", "), text);
    }

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unban &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("unbanned", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Untimeout &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("untimedout", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Delete &action)
{
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);

    (void)event;
    builder->loginName.clear();
    QString text;
    builder.emplace<TextElement>("A message from", MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(action.userName.qt(),
                              MessageElementFlag::Username,
                              MessageColor::System, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, action.userLogin.qt()});
    builder.emplace<TextElement>("was deleted:", MessageElementFlag::Text,
                                 MessageColor::System);

    auto limit = getSettings()->deletedMessageLengthLimit.getValue();
    if (limit > 0 && action.messageBody.view().length() > limit)
    {
        builder
            .emplace<TextElement>(action.messageBody.qt().left(limit) + "…",
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt().left(limit) + "…");
    }
    else
    {
        builder
            .emplace<TextElement>(action.messageBody.qt(),
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt());
    }

    text = QString("A message from %1 was deleted: %2")
               .arg(action.userLogin.qt(), text);
    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Followers &action)
{
    QString duration;
    if (action.followDurationMinutes > 0)
    {
        duration = u"(%1 minutes)"_s.arg(action.followDurationMinutes);
    }
    makeModeMessage(builder, event, u"followers-only"_s, true, duration);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::FollowersOff & /*action*/)
{
    makeModeMessage(builder, event, u"followers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnly & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnlyOff & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, false);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Slow &action)
{
    makeModeMessage(builder, event, u"slow"_s, true,
                    u"(%1 seconds)"_s.arg(action.waitTimeSeconds));
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SlowOff & /*action*/)
{
    makeModeMessage(builder, event, u"slow"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Subscribers & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SubscribersOff & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Uniquechat & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UniquechatOff & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::AutomodTerms &action)
{
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.action == "add")
    {
        builder.emplaceSystemTextAndUpdate(u"added"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
    }

    QString terms;
    for (size_t i = 0; i < action.terms.size(); i++)
    {
        if (i != 0)
        {
            if (i == action.terms.size() - 1)
            {
                if (action.terms.size() == 2)
                {
                    terms.append(u" and ");
                }
                else
                {
                    terms.append(u", and ");
                }
            }
            else
            {
                terms.append(u", ");
            }
        }
        terms.append(u'"');
        terms.append(action.terms[i].qt());
        terms.append(u'"');
    }
    builder.emplaceSystemTextAndUpdate(terms, text);
    builder.emplaceSystemTextAndUpdate(u"as"_s, text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"a"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(action.list.qt(), text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"term"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"terms"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(u"on AutoMod."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Mod &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"modded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unmod &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"unmodded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Raid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("initiated a raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unraid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("canceled the raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UnbanRequest &action)
{
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.isApproved)
    {
        builder.emplaceSystemTextAndUpdate("approved", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate("denied", text);
    }
    builder.appendOrEmplaceSystemTextAndUpdate("the unban request from", text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    if (action.moderatorMessage.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.appendOrEmplaceSystemTextAndUpdate(action.moderatorMessage.qt(),
                                                   text);
    }

    builder.setMessageAndSearchText(text);
}

automod::HoldData makeAutoModReviewHoldData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_hold::v2::Event &event)
{
    automod::HoldData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = event.userID.qt();
    data.userLogin = event.userLogin.qt();
    data.userName = event.userName.qt();
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

automod::UpdateData makeAutoModReviewUpdateData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_update::v2::Event &event)
{
    automod::UpdateData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = QString::fromStdString(event.userID);
    data.userLogin = QString::fromStdString(event.userLogin);
    data.userName = QString::fromStdString(event.userName);
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    data.status = QString::fromStdString(event.status);
    data.moderatorID = QString::fromStdString(event.moderatorUserID);
    data.moderatorLogin = QString::fromStdString(event.moderatorUserLogin);
    data.moderatorName = QString::fromStdString(event.moderatorUserName);
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

MessagePtr makeAutoModReviewMessage(TwitchChannel *channel,
                                    const automod::ReviewItem &item,
                                    AutoModReviewPresentation presentation)
{
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = item.receivedAt;
    builder->id = u"automod_" % item.messageID;
    builder->loginName = item.userLogin;
    builder->displayName = item.userName;
    builder->userID = item.userID;
    builder->channelName = item.broadcasterLogin;
    builder->flags.set(MessageFlag::PubSub, MessageFlag::EventSub,
                       MessageFlag::ModerationAction, MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessageHeader,
                       MessageFlag::AutoModOffendingMessage,
                       MessageFlag::InvalidReplyTarget);
    builder->flags.set(MessageFlag::AutoModBlockedTerm, item.blockedTerm);
    builder->autoModReview = std::make_shared<const automod::ReviewItem>(item);
    builder->highlightMatches = makeAutoModMatches(item);
    // where the text of the held message is among the elements
    size_t heldBegin = 0;
    size_t heldEnd = 0;

    const auto userColor =
        channel != nullptr ? channel->getUserColor(item.userLogin) : QColor{};
    const auto displayName = localizedDisplayName(item);
    const bool pending = item.state == automod::ReviewState::Approving ||
                         item.state == automod::ReviewState::Denying ||
                         item.secondaryInProgress;
    const bool showActionRow =
        item.actionsAllowed && item.state == automod::ReviewState::Open;
    const bool failed = item.actionsAllowed &&
                        (item.failedAction != automod::ReviewAction::None ||
                         item.secondaryFailed);

    // What there is to say about a message nothing can be done with right now
    const auto addStatus = [&](FontStyle style) {
        if (pending)
        {
            builder.emplace<TextElement>(
                automod::reviewStateLabel(item), MessageElementFlag::Text,
                MessageColor::System,
                style == FontStyle::ChatSmall ? FontStyle::ChatSmall
                                              : FontStyle::ChatMediumBold);
        }
        else if (item.state == automod::ReviewState::Open &&
                 !item.actionsAllowed)
        {
            builder.emplace<TextElement>(u"Read only"_s,
                                         MessageElementFlag::Text,
                                         MessageColor::System, style);
        }
        else if (failed)
        {
            auto failureText = automod::reviewActionLabel(
                item.secondaryFailed ? item.secondaryAction
                                     : item.failedAction);
            if (!failureText.isEmpty())
            {
                failureText[0] = failureText.at(0).toUpper();
            }
            failureText += u" failed"_s;
            if (!item.statusDetail.isEmpty())
            {
                failureText += u" · "_s % item.statusDetail;
            }
            builder
                .emplace<TextElement>(failureText, MessageElementFlag::Text,
                                      MessageColor::System, style)
                ->setTooltip(item.statusDetail);
            builder
                .emplace<TextElement>(u"Retry"_s, MessageElementFlag::Text,
                                      MessageColor::Link,
                                      FontStyle::ChatMediumBold)
                ->setLink({Link::AutoModReviewRetry, item.key})
                ->setTooltip(u"Try the failed action again"_s);
        }
        else if (item.state == automod::ReviewState::Approved ||
                 item.state == automod::ReviewState::Denied)
        {
            QString resolution = item.state == automod::ReviewState::Approved
                                     ? u"Approved"_s
                                     : u"Denied"_s;
            const auto moderator = item.moderatorName.isEmpty()
                                       ? item.moderatorLogin
                                       : item.moderatorName;
            if (!moderator.isEmpty())
            {
                resolution += u" by "_s % moderator;
            }
            if (item.state == automod::ReviewState::Denied &&
                !item.secondaryInProgress &&
                (item.secondaryAction == automod::ReviewAction::Timeout ||
                 item.secondaryAction == automod::ReviewAction::Ban) &&
                !item.statusDetail.isEmpty())
            {
                // the timeout or ban that followed
                resolution += u" · "_s % item.statusDetail;
            }
            builder.emplace<TextElement>(resolution, MessageElementFlag::Text,
                                         MessageColor::System, style);
        }
        else
        {
            const auto status = item.statusDetail.isEmpty()
                                    ? automod::reviewStateLabel(item)
                                    : item.statusDetail;
            builder.emplace<TextElement>(status, MessageElementFlag::Text,
                                         MessageColor::System, style);
        }
    };

    if (presentation == AutoModReviewPresentation::ChatLegacy)
    {
        // The compact layout: what AutoMod says on one line, the held
        // message on the next.
        builder.emplace<BadgeElement>(
            makeAutoModBadge(), MessageElementFlag::BadgeChannelAuthority);
        builder.emplace<TextElement>(u"AutoMod:"_s, MessageElementFlag::Text,
                                     QColor(0, 0, 255),
                                     FontStyle::ChatMediumBold);
        builder.emplace<TextElement>(
            u"Held a message for reason: "_s % item.reasonSummary % u'.',
            MessageElementFlag::Text, MessageColor::Text);

        if (showActionRow)
        {
            builder
                .emplace<TextElement>(u"Allow"_s, MessageElementFlag::Text,
                                      MessageColor(QColor(0, 255, 0)),
                                      FontStyle::ChatMediumBold)
                ->setLink({Link::AutoModReviewApprove, item.key})
                ->setTooltip(u"Allow this message"_s);
            builder
                .emplace<TextElement>(u"Deny"_s, MessageElementFlag::Text,
                                      MessageColor(QColor(255, 0, 0)),
                                      FontStyle::ChatMediumBold)
                ->setLink({Link::AutoModReviewDeny, item.key})
                ->setTooltip(u"Deny this message"_s);
        }
        else
        {
            addStatus(FontStyle::ChatMedium);
        }

        builder.emplace<LinebreakElement>(MessageElementFlag::Text);
        builder
            .emplace<TextElement>(u'#' + item.broadcasterLogin,
                                  MessageElementFlag::ChannelName,
                                  MessageColor::System)
            ->setLink({Link::JumpToChannel, item.broadcasterLogin});
        builder.emplace<TimestampElement>(item.receivedAt.time());
        builder.emplace<TwitchModerationElement>();
        builder.emplace<MentionElement>(displayName + ':', item.userLogin,
                                        MessageColor::Text, userColor);
        heldBegin = builder->elements.size();
        appendHeldMessageText(builder, channel, item.messageText);
        heldEnd = builder->elements.size();

        builder->messageText = item.messageText;
        builder->searchText = item.broadcasterLogin % u' ' % item.userLogin %
                              u' ' % item.messageText % u' ' %
                              item.reasonSummary % u' ' %
                              automod::reviewStateLabel(item);
        ignoreMatchesOutsideHeldText(builder, heldBegin, heldEnd);
        return builder.release();
    }

    // The advanced layout: the reason, the held message, and beneath it
    // everything that can be done with it.
    const bool isReviewQueue =
        presentation == AutoModReviewPresentation::ReviewQueue;

    const auto reviewReason = [&item] {
        if (item.blockedTerm)
        {
            return item.reasonSummary;
        }

        auto reason = item.reasonCategory;
        if (!reason.isEmpty() && item.reasonLevel > 0)
        {
            reason += u" · Level "_s % QString::number(item.reasonLevel);
        }
        return reason;
    }();
    const bool showReason =
        getSettings()->autoModReviewShowReason && !reviewReason.isEmpty();

    if (isReviewQueue)
    {
        // messages of all channels end up here, so say where it is from
        builder
            .emplace<TextElement>(u'#' + item.broadcasterLogin,
                                  MessageElementFlag::Text, MessageColor::Link,
                                  FontStyle::ChatSmall)
            ->setLink({Link::JumpToChannel, item.broadcasterLogin});
        if (showReason)
        {
            builder.emplace<TextElement>(u"·"_s, MessageElementFlag::Text,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
        }
    }
    if (showReason)
    {
        builder.emplace<TextElement>(reviewReason, MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
    }
    if (isReviewQueue || showReason)
    {
        builder.emplace<LinebreakElement>(MessageElementFlag::Text);
    }

    builder.emplace<TimestampElement>(item.receivedAt.time());
    builder.emplace<MentionElement>(displayName + ':', item.userLogin,
                                    MessageColor::Text, userColor);
    heldBegin = builder->elements.size();
    appendHeldMessageText(builder, channel, item.messageText);
    heldEnd = builder->elements.size();

    // Shown while the message is selected in the queue
    const MessageElementFlags expandedFlags{
        MessageElementFlag::Text, MessageElementFlag::AutoModReviewExpanded};
    const MessageElementFlags contextFlags =
        isReviewQueue ? expandedFlags
                      : MessageElementFlags{MessageElementFlag::Text};
    if (getSettings()->autoModReviewShowContext && !item.context.isEmpty())
    {
        builder.emplace<LinebreakElement>(contextFlags);
        builder.emplace<TextElement>(u"Earlier messages"_s, contextFlags,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
        const auto addContext = [&](const automod::ContextMessage &context) {
            builder.emplace<LinebreakElement>(contextFlags);
            builder.emplace<TextElement>(context.displayName + ':',
                                         contextFlags, MessageColor::Link,
                                         FontStyle::ChatSmall);
            builder.emplace<TextElement>(context.text, contextFlags,
                                         MessageColor::Text,
                                         FontStyle::ChatSmall);
        };
        if (getSettings()->autoModReviewContextOrder == 1)
        {
            // oldest at the top
            for (const auto &context : item.context)
            {
                addContext(context);
            }
        }
        else
        {
            for (auto it = item.context.crbegin(); it != item.context.crend();
                 ++it)
            {
                addContext(*it);
            }
        }
    }

    builder.emplace<LinebreakElement>(MessageElementFlag::Text);
    if (showActionRow)
    {
        builder
            .emplace<TextElement>(u"Approve"_s, MessageElementFlag::Text,
                                  MessageColor(QColor(0, 255, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewApprove, item.key})
            ->setTooltip(u"Approve this message"_s);
        builder
            .emplace<TextElement>(u"Deny"_s, MessageElementFlag::Text,
                                  MessageColor(QColor(255, 0, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewDeny, item.key})
            ->setTooltip(u"Deny this message"_s);
        builder.emplace<TextElement>(u"·"_s, MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);

        const auto addTimeout = [&](int seconds, const QString &tooltip) {
            // the key and the duration, see ChannelView::handleLinkClick
            const QString value = item.key % u'\n' % QString::number(seconds);
            builder
                .emplace<AutoModActionElement>(AutoModActionKind::Timeout,
                                               seconds)
                ->setLink({Link::AutoModReviewTimeout, value})
                ->setTooltip(tooltip);
        };
        addTimeout(30, u"Deny and timeout for 30 seconds"_s);
        addTimeout(60, u"Deny and timeout for 1 minute"_s);
        addTimeout(5 * 60, u"Deny and timeout for 5 minutes"_s);
        addTimeout(10 * 60, u"Deny and timeout for 10 minutes"_s);
        addTimeout(60 * 60, u"Deny and timeout for 1 hour"_s);
        addTimeout(24 * 60 * 60, u"Deny and timeout for 1 day"_s);
        builder
            .emplace<TextElement>(u"…"_s, MessageElementFlag::Text,
                                  MessageColor::Link, FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewTimeout, item.key})
            ->setTooltip(u"Deny and timeout for another duration"_s);
        builder.emplace<AutoModActionElement>(AutoModActionKind::Ban)
            ->setLink({Link::AutoModReviewBan, item.key})
            ->setTooltip(u"Deny and permanently ban this user"_s);
    }
    else
    {
        addStatus(FontStyle::ChatSmall);
    }

    if (isReviewQueue && getSettings()->autoModReviewShowShortcutHints)
    {
        builder.emplace<LinebreakElement>(expandedFlags);
        const auto addHint = [&](const QString &key, const QString &action,
                                 bool separator) {
            builder.emplace<TextElement>(key, expandedFlags, MessageColor::Link,
                                         FontStyle::ChatSmall);
            builder.emplace<TextElement>(action, expandedFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
            if (separator)
            {
                builder.emplace<TextElement>(u"·"_s, expandedFlags,
                                             MessageColor::System,
                                             FontStyle::ChatSmall);
            }
        };
        addHint(u"↑/↓"_s, u"Select"_s, true);
        if (showActionRow)
        {
            addHint(u"A"_s, u"Approve"_s, true);
            addHint(u"D"_s, u"Deny"_s, true);
            addHint(u"T"_s, u"Timeout"_s, true);
            addHint(u"B"_s, u"Ban"_s, true);
        }
        else if (failed)
        {
            addHint(u"R"_s, u"Retry"_s, true);
        }
        addHint(u"Enter"_s, u"Usercard"_s, false);
    }

    builder->messageText = item.messageText;
    builder->searchText = item.broadcasterLogin % u' ' % item.userLogin % u' ' %
                          item.messageText % u' ' % item.reasonSummary % u' ' %
                          automod::reviewStateLabel(item);
    ignoreMatchesOutsideHeldText(builder, heldBegin, heldEnd);
    return builder.release();
}

MessagePtr makeSuspiciousUserMessageHeader(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_message::v1::Event &event)
{
    EventSubMessageBuilder builder(channel);

    // Builder for low trust user message with explanation
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->serverReceivedTime = time;
    builder->flags.set(MessageFlag::LowTrustUsers);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);

    // Suspicious user header message
    QString prefix = u"Suspicious User:"_s;
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor(QColor(0, 0, 255)),
                                 FontStyle::ChatMediumBold);

    QString headerMessage;
    if (event.lowTrustStatus == lib::suspicious_users::Status::Restricted)
    {
        headerMessage = u"Restricted"_s;
        builder->flags.set(MessageFlag::RestrictedMessage);
    }
    else
    {
        headerMessage = u"Monitored"_s;
        builder->flags.set(MessageFlag::MonitoredMessage);
    }

    auto hasType = [&](lib::suspicious_users::Type type) {
        return std::ranges::find(event.types, type) != event.types.end();
    };

    if (hasType(lib::suspicious_users::Type::BanEvaderDetector))
    {
        QString evader;
        if (event.banEvasionEvaluation ==
            lib::suspicious_users::BanEvasionEvaluation::Likely)
        {
            evader = u"likely"_s;
        }
        else
        {
            evader = u"possible"_s;
        }

        headerMessage += QStringLiteral(". Detected as ") % evader %
                         QStringLiteral(" ban evader");
    }

    if (hasType(lib::suspicious_users::Type::SharedChannelBan))
    {
        headerMessage += QStringLiteral(". Banned in ") %
                         QString::number(event.sharedBanChannelIds.size()) %
                         QStringLiteral(" shared channels");
    }

    builder.emplace<TextElement>(headerMessage, MessageElementFlag::Text,
                                 MessageColor::Text);

    builder.setMessageAndSearchText(prefix % u" " % headerMessage);

    return builder.release();
}

MessagePtr makeSuspiciousUserMessageBody(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_message::v1::Event &event)
{
    EventSubMessageBuilder builder(channel);
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->serverReceivedTime = time;
    if (event.lowTrustStatus == lib::suspicious_users::Status::Restricted)
    {
        builder->flags.set(MessageFlag::RestrictedMessage);
    }
    else
    {
        builder->flags.set(MessageFlag::MonitoredMessage);
    }

    builder
        .emplace<TextElement>(u'#' + event.broadcasterUserLogin.qt(),
                              MessageElementFlag::ChannelName,
                              MessageColor::System)
        ->setLink({Link::JumpToChannel, event.broadcasterUserLogin.qt()});
    builder.emplace<TimestampElement>(time.time());
    builder.emplace<TwitchModerationElement>();
    builder->loginName = event.userLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::LowTrustUsers);

    // sender username
    builder.emplace<MentionElement>(
        event.userName.qt() + ":", event.userLogin.qt(), MessageColor::Text,
        channel->getUserColor(event.userLogin.qt()));

    // sender's message caught by AutoMod
    // XXX: add the structured message here
    builder.emplace<TextElement>(event.message.text.qt(),
                                 MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(event.userName.qt() % u": " %
                                    event.message.text.qt());

    return builder.release();
}

MessagePtr makeSuspiciousUserUpdate(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_update::v1::Event &event)
{
    EventSubMessageBuilder builder(channel, time);
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);
    builder->loginName = event.moderatorUserLogin.qt();

    QString text;
    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);

    switch (event.lowTrustStatus)
    {
        case lib::suspicious_users::Status::None: {
            builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"from the suspicious user list."_s, text);
        }
        break;

        case lib::suspicious_users::Status::ActiveMonitoring: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a monitored suspicious chatter."_s, text);
        }
        break;

        case lib::suspicious_users::Status::Restricted: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a restricted suspicious chatter."_s, text);
        }
        break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageHeldMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_hold::v1::Event &event)
{
    QString text("AutoMod: Hey! Your message is being checked by mods and has "
                 "not been sent.");
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(
        "Hey! Your message is being checked by mods and has not been sent.",
        MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageUpdateMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_update::v1::Event &event)
{
    using lib::payload::channel_chat_user_message_update::v1::Status;

    QString text("AutoMod: ");
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);

    switch (event.status)
    {
        case Status::Approved:
            text += "Mods have accepted your message.";
            builder.emplace<TextElement>("Mods have accepted your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Denied:
            text += "Mods have denied your message.";
            builder.emplace<TextElement>("Mods have denied your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Invalid:
            text += "Your message was lost in the void.";
            builder.emplace<TextElement>("Your message was lost in the void.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtrMut makeFollowMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_follow::v2::Event &event)
{
    EventSubMessageBuilder builder(channel, time);
    QString text;
    builder.appendUser(event.userName, event.userLogin, text);
    builder.emplaceSystemTextAndUpdate("followed the channel.", text);
    builder.setMessageAndSearchText(text);
    auto msg = builder.release();
    msg->flags.set(MessageFlag::Follow);
    return msg;
}

}  // namespace chatterino::eventsub
