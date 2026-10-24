// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/HighlightTesterDialog.hpp"

#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightMatch.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "controllers/highlights/types/AnyHighlight.hpp"
#include "controllers/highlights/types/Common.hpp"
#include "messages/Message.hpp"
#include "messages/MessageParseArgs.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/Variant.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace chatterino {

namespace {

/// The scope of a highlight that can be limited to channels
const highlights::HighlightExtras *channelExtras(
    const highlights::AnyHighlight &h)
{
    if (const auto *m = std::get_if<highlights::MessageHighlight>(&h))
    {
        return &m->extras;
    }
    if (const auto *u = std::get_if<highlights::UserHighlight>(&h))
    {
        return &u->extras;
    }
    return nullptr;
}

/// The same highlight, applying in every channel
highlights::AnyHighlight withoutChannelScope(highlights::AnyHighlight h)
{
    if (auto *m = std::get_if<highlights::MessageHighlight>(&h))
    {
        m->extras.setChannelScope({});
    }
    if (auto *u = std::get_if<highlights::UserHighlight>(&h))
    {
        u->extras.setChannelScope({});
    }
    return h;
}

HighlightCheck buildCheck(const highlights::AnyHighlight &h)
{
    return std::visit(
        [](const auto &highlight) {
            return highlight.buildCheck();
        },
        h);
}

}  // namespace

HighlightTesterDialog::HighlightTesterDialog(QWidget *parent)
    : QDialog(parent)
{
    this->setWindowTitle("Test highlights");
    this->setMinimumSize(650, 520);
    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout();
    this->sender_ = new QLineEdit("someone", this);
    this->channel_ = new QLineEdit(this);
    this->channel_->setPlaceholderText("Optional channel name");
    this->platform_ = new QComboBox(this);
    this->platform_->addItem("Twitch",
                             static_cast<int>(MessagePlatform::AnyOrTwitch));
    this->platform_->addItem("YouTube",
                             static_cast<int>(MessagePlatform::YouTube));
    this->platform_->addItem("Kick", static_cast<int>(MessagePlatform::Kick));
    form->addRow("Sender:", this->sender_);
    form->addRow("Channel:", this->channel_);
    form->addRow("Platform:", this->platform_);
    layout->addLayout(form);

    this->message_ = new QPlainTextEdit(this);
    this->message_->setPlaceholderText("Type or paste a test chat message");
    layout->addWidget(this->message_, 1);
    layout->addWidget(new QLabel("Preview:", this));
    this->preview_ = new QTextEdit(this);
    this->preview_->setReadOnly(true);
    this->preview_->setMaximumHeight(90);
    layout->addWidget(this->preview_);
    layout->addWidget(new QLabel("Match details:", this));
    this->explanation_ = new QPlainTextEdit(this);
    this->explanation_->setReadOnly(true);
    layout->addWidget(this->explanation_, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);

    const auto update = [this] {
        this->updateResult();
    };
    QObject::connect(this->message_, &QPlainTextEdit::textChanged, this,
                     update);
    QObject::connect(this->sender_, &QLineEdit::textChanged, this, update);
    QObject::connect(this->channel_, &QLineEdit::textChanged, this, update);
    QObject::connect(this->platform_, &QComboBox::currentIndexChanged, this,
                     update);
    this->updateResult();
    this->resize(this->minimumSize());
}

void HighlightTesterDialog::updateResult()
{
    const auto text = this->message_->toPlainText();
    this->preview_->setPlainText(text);
    if (text.isEmpty())
    {
        this->explanation_->setPlainText(
            "Enter a message to see which highlights match and why.");
        return;
    }
    const auto sender = this->sender_->text().trimmed();
    if (getSettings()->isBlacklistedUser(sender))
    {
        this->explanation_->setPlainText(
            "No highlight: this sender is in Blacklisted Users.");
        return;
    }

    const auto platform =
        static_cast<MessagePlatform>(this->platform_->currentData().toInt());
    const auto channel = this->channel_->text().trimmed();

    auto message = std::make_shared<Message>();
    message->messageText = text;
    message->loginName = sender;
    message->platform = platform;
    message->channelName = channel;
    const MessageParseArgs args;
    const std::vector<TwitchBadge> noBadges;
    const MessageFlags noFlags;
    const HighlightCheckParams params{
        .args = args,
        .twitchBadges = noBadges,
        .senderName = sender,
        .originalMessage = text,
        .messageFlags = noFlags,
        .self = false,
        .runContext =
            filters::RunContext{
                .message = *message,
                .channel = nullptr,
            },
        .platform = platform,
        .channelName = channel,
    };

    QStringList matched;
    QStringList scopeRejections;
    std::vector<HighlightMatch> matches;
    const auto highlights = getSettings()->sharedHighlights.readOnly();
    for (const auto &highlight : *highlights)
    {
        if (!highlights::isEnabled(highlight))
        {
            continue;
        }
        const auto check = buildCheck(highlight);
        if (!check)
        {
            continue;
        }

        const auto name = highlights::getName(highlight);
        if (auto result = check(params))
        {
            QString mode;
            if (const auto *m =
                    std::get_if<highlights::MessageHighlight>(&highlight))
            {
                mode =
                    QStringLiteral(" (%1, %2)")
                        .arg(m->isRegex() ? QStringLiteral("regular expression")
                                          : QStringLiteral("literal phrase"),
                             m->isCaseSensitive()
                                 ? QStringLiteral("case sensitive")
                                 : QStringLiteral("case insensitive"));
            }
            matched.append(QStringLiteral("%1 highlight \"%2\"%3 matched.")
                               .arg(highlights::getHighlightTypeName(highlight),
                                    name, mode));
            matches.insert(matches.end(), result->matches.begin(),
                           result->matches.end());
            continue;
        }

        // Would it have matched in every channel?
        const auto *extras = channelExtras(highlight);
        if (extras == nullptr)
        {
            continue;
        }
        const auto scope = extras->getChannelScope();
        if (scope.mode() == HighlightChannelScopeMode::Everywhere)
        {
            continue;
        }
        const auto unscoped = buildCheck(withoutChannelScope(highlight));
        if (unscoped && unscoped(params))
        {
            const auto context =
                channel.isEmpty()
                    ? QStringLiteral("no channel was entered")
                    : QStringLiteral("%1 channel \"%2\"")
                          .arg(this->platform_->currentText(), channel);
            scopeRejections.append(
                QStringLiteral("%1 highlight \"%2\" matched, but where it "
                               "applies does not include this test because "
                               "%3. Applies in: %4.")
                    .arg(highlights::getHighlightTypeName(highlight), name,
                         context,
                         highlightChannelScopeSummary(scope.mode(),
                                                      scope.targets().size())));
        }
    }

    QStringList explanation;
    if (matched.isEmpty())
    {
        explanation.append(scopeRejections.isEmpty()
                               ? QStringLiteral("No highlights matched.")
                               : QStringLiteral("No highlight was applied."));
    }
    explanation.append(matched);

    auto *document = this->preview_->document();
    for (const auto &match : matches)
    {
        QTextCursor cursor(document);
        cursor.setPosition(static_cast<int>(match.start));
        cursor.setPosition(static_cast<int>(match.start + match.length),
                           QTextCursor::KeepAnchor);
        if (match.style != HighlightMatchStyle::None)
        {
            QTextCharFormat format;
            format.setBackground(match.color);
            cursor.mergeCharFormat(format);
        }

        const auto matchedText =
            text.mid(match.start, match.length).replace(u'\n', u' ');
        explanation.append(
            QStringLiteral("Pattern \"%1\" matched \"%2\" at characters %3 "
                           "through %4.")
                .arg(match.pattern, matchedText)
                .arg(match.start + 1)
                .arg(match.start + match.length));
    }
    if (!matched.isEmpty() && matches.empty())
    {
        explanation.append(
            "No part of the text is marked: the highlights that matched do "
            "not look at the text, or their marker is off.");
    }
    explanation.append(scopeRejections);
    this->explanation_->setPlainText(explanation.join(u'\n'));
}

}  // namespace chatterino
