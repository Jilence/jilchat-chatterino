// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkInfo.hpp"

#include "debug/AssertInGuiThread.hpp"

#include <QString>

namespace chatterino {

namespace {

/// Characters after which a long run (a URL) may break
bool isBreakAfter(QChar c)
{
    switch (c.unicode())
    {
        case u'/':
        case u'?':
        case u'&':
        case u'=':
        case u'%':
        case u'-':
        case u'_':
        case u'.':
        case u',':
        case u';':
            return true;
        default:
            return false;
    }
}

/// Lets the tooltip wrap long words, like the URL of an unshortened link:
/// a label only breaks at spaces, so a long URL without any made the tooltip
/// wider than the screen. Adds zero-width spaces to the text (never inside
/// tags or entities) after URL separators, and every so many characters
/// where there are none.
QString allowLineBreaks(const QString &html)
{
    constexpr qsizetype breakSeparatorAfter = 24;
    constexpr qsizetype breakAnywhereAfter = 48;
    const QChar zeroWidthSpace(0x200B);

    QString out;
    out.reserve(html.size() + (html.size() / breakSeparatorAfter));
    bool inTag = false;
    bool inEntity = false;
    qsizetype run = 0;
    for (const auto c : html)
    {
        out.append(c);
        if (inTag)
        {
            inTag = c != u'>';
            continue;
        }
        if (c == u'<')
        {
            inTag = true;
            run = 0;
            continue;
        }
        if (inEntity)
        {
            inEntity = c != u';';
            if (!inEntity)
            {
                ++run;
            }
            continue;
        }
        if (c == u'&')
        {
            inEntity = true;
            continue;
        }
        if (c.isSpace())
        {
            run = 0;
            continue;
        }
        ++run;
        if ((run >= breakSeparatorAfter && isBreakAfter(c)) ||
            run >= breakAnywhereAfter)
        {
            out.append(zeroWidthSpace);
            run = 0;
        }
    }
    return out;
}

}  // namespace

LinkInfo::LinkInfo(QString url)
    : QObject(nullptr)
    , originalUrl_(url)
    , url_(std::move(url))
    , tooltip_(this->url_)
{
}

LinkInfo::~LinkInfo() = default;

LinkInfo::State LinkInfo::state() const
{
    return this->state_;
}

QString LinkInfo::url() const
{
    return this->url_;
}

QString LinkInfo::originalUrl() const
{
    return this->originalUrl_;
}

bool LinkInfo::isPending() const
{
    return this->state_ == State::Created;
}

bool LinkInfo::isLoading() const
{
    return this->state_ == State::Loading;
}

bool LinkInfo::isLoaded() const
{
    return this->state_ > State::Loading;
}

bool LinkInfo::isResolved() const
{
    return this->state_ == State::Resolved;
}

bool LinkInfo::hasError() const
{
    return this->state_ == State::Errored;
}

bool LinkInfo::hasThumbnail() const
{
    return this->thumbnail_ && !this->thumbnail_->url().string.isEmpty();
}

QString LinkInfo::tooltip() const
{
    return this->tooltip_;
}

ImagePtr LinkInfo::thumbnail() const
{
    return this->thumbnail_;
}

void LinkInfo::setState(State state)
{
    assertInGuiThread();
    assert(state >= this->state_);

    if (this->state_ == state)
    {
        return;
    }

    this->state_ = state;
    this->stateChanged(state);
}

void LinkInfo::setResolvedUrl(QString resolvedUrl)
{
    assertInGuiThread();
    this->url_ = std::move(resolvedUrl);
}

void LinkInfo::setTooltip(QString tooltip)
{
    assertInGuiThread();
    this->tooltip_ = allowLineBreaks(tooltip);
}

void LinkInfo::setThumbnail(ImagePtr thumbnail)
{
    assertInGuiThread();
    this->thumbnail_ = std::move(thumbnail);
}

}  // namespace chatterino
