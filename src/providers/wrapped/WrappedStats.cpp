// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/wrapped/WrappedStats.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "widgets/dialogs/WrappedDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace chatterino::wrapped {

namespace {

/// Keeps a year and a bit, so last year's Wrapped still works in January
constexpr int KEEP_DAYS = 400;
/// Keeps one day's file entry small even in very busy chats
constexpr int MAX_KEYS_PER_DAY = 300;
/// Pictures are only needed for the emotes that make it into a top list
constexpr int MAX_EMOTE_URLS = 1000;

QString statsPath()
{
    return getApp()->getPaths().settingsDirectory + "/jilchat-wrapped.json";
}

void bump(QHash<QString, int> &counts, const QString &key, int by = 1)
{
    if (key.isEmpty())
    {
        return;
    }
    auto it = counts.find(key);
    if (it != counts.end())
    {
        it.value() += by;
    }
    else if (counts.size() < MAX_KEYS_PER_DAY)
    {
        counts.insert(key, by);
    }
}

QJsonObject toJson(const QHash<QString, int> &counts)
{
    QJsonObject out;
    for (auto it = counts.begin(); it != counts.end(); ++it)
    {
        out.insert(it.key(), it.value());
    }
    return out;
}

QHash<QString, int> fromJson(const QJsonValue &value)
{
    QHash<QString, int> out;
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        out.insert(it.key(), it.value().toInt());
    }
    return out;
}

WrappedStats::Ranking top(const QHash<QString, int> &counts, size_t limit)
{
    WrappedStats::Ranking ranking;
    ranking.reserve(size_t(counts.size()));
    for (auto it = counts.begin(); it != counts.end(); ++it)
    {
        ranking.emplace_back(it.key(), it.value());
    }
    std::sort(ranking.begin(), ranking.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranking.size() > limit)
    {
        ranking.resize(limit);
    }
    return ranking;
}

QString currentLogin()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    return account == nullptr || account->isAnon()
               ? QString()
               : account->getUserName().toLower();
}

}  // namespace

WrappedStats &WrappedStats::instance()
{
    static auto *stats = new WrappedStats();
    return *stats;
}

WrappedStats::WrappedStats()
{
    this->load();

    this->minuteTimer_.setInterval(60 * 1000);
    QObject::connect(&this->minuteTimer_, &QTimer::timeout, this, [this] {
        this->countMinute();
    });

    // Saved every couple of minutes, so a crash loses very little
    this->saveTimer_.setSingleShot(true);
    this->saveTimer_.setInterval(2 * 60 * 1000);
    QObject::connect(&this->saveTimer_, &QTimer::timeout, this, [this] {
        this->save();
    });
}

void WrappedStats::start()
{
    if (!this->started_)
    {
        this->started_ = true;
        this->minuteTimer_.start();

        // Checks soon after starting, then every hour in case the app is
        // left open across the 1st
        this->announceTimer_.setInterval(60 * 60 * 1000);
        QObject::connect(&this->announceTimer_, &QTimer::timeout, this, [this] {
            this->announceIfDue();
        });
        this->announceTimer_.start();
        QTimer::singleShot(8000, this, [this] {
            this->announceIfDue();
        });
    }
}

void WrappedStats::announceIfDue()
{
    const auto today = QDate::currentDate();
    const auto month = today.toString(u"yyyy-MM");
    if (getSettings()->wrappedAnnouncedMonth.getValue() == month)
    {
        return;
    }
    getSettings()->wrappedAnnouncedMonth = month;
    getSettings()->requestSave();

    // Last month is the one that's ready. A month with nothing in it is skipped.
    const QDate lastMonth = QDate(today.year(), today.month(), 1).addMonths(-1);
    if (this->summarize(lastMonth, lastMonth.addMonths(1).addDays(-1)).empty())
    {
        return;
    }
    WrappedBanner::announce();
}

QString WrappedStats::emoteUrl(const QString &name) const
{
    return this->emoteUrls_.value(name);
}

WrappedStats::Day &WrappedStats::today()
{
    const auto date = QDate::currentDate();
    if (!this->since_.isValid())
    {
        this->since_ = date;
    }
    return this->days_[date];
}

void WrappedStats::markDirty()
{
    this->dirty_ = true;
    if (!this->saveTimer_.isActive())
    {
        this->saveTimer_.start();
    }
}

void WrappedStats::recordMessage(const QString &channelName,
                                 const MessagePtr &message)
{
    if (message == nullptr || message->loginName.isEmpty())
    {
        return;
    }
    postToThread([this, channel = channelName.toLower(), message] {
        const auto login = currentLogin();
        if (login.isEmpty())
        {
            return;
        }
        auto &day = this->today();

        if (message->loginName.compare(login, Qt::CaseInsensitive) == 0)
        {
            bump(day.messages, channel);
            day.hours[size_t(QTime::currentTime().hour())] += 1;

            QSet<QString> counted;
            for (const auto &element : message->elements)
            {
                if (const auto *emote =
                        dynamic_cast<const EmoteElement *>(element.get()))
                {
                    if (const auto data = emote->getEmote())
                    {
                        // A word repeated in one message counts once
                        const auto name = data->name.string;
                        if (!counted.contains(name))
                        {
                            counted.insert(name);
                            bump(day.emotes, name);
                            // The sharpest picture, for showing it in Wrapped
                            for (const auto &image : {data->images.getImage3(),
                                                      data->images.getImage2(),
                                                      data->images.getImage1()})
                            {
                                if (image != nullptr && !image->isEmpty() &&
                                    !image->url().string.isEmpty())
                                {
                                    if (this->emoteUrls_.contains(name) ||
                                        this->emoteUrls_.size() <
                                            MAX_EMOTE_URLS)
                                    {
                                        this->emoteUrls_.insert(
                                            name, image->url().string);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // "@someone hi" counts as talking to them
            const auto text = message->messageText.trimmed();
            if (text.startsWith(u'@'))
            {
                auto name = text.mid(1).section(u' ', 0, 0).toLower();
                name.remove(u',');
                bump(day.talkedTo, name);
            }
            this->markDirty();
        }
        else if (login.size() >= 3 &&
                 message->messageText.contains(login, Qt::CaseInsensitive))
        {
            day.mentions += 1;
            bump(day.mentionedBy, message->loginName.toLower());
            this->markDirty();
        }
    });
}

void WrappedStats::countMinute()
{
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }

    // Each chat you can see counts once, even if it's open in two places
    QSet<QString> visible;
    for (auto *window : app->getWindows()->windows())
    {
        if (window == nullptr || !window->isVisible() || window->isMinimized())
        {
            continue;
        }
        auto *page = window->getNotebook().getSelectedPage();
        if (page == nullptr)
        {
            continue;
        }
        for (auto *split : page->getSplits())
        {
            const auto channel = split->getChannel();
            if (channel != nullptr &&
                channel->getType() == Channel::Type::Twitch)
            {
                visible.insert(channel->getName().toLower());
            }
        }
    }
    if (visible.isEmpty())
    {
        return;
    }
    auto &day = this->today();
    for (const auto &name : visible)
    {
        bump(day.minutes, name);
    }
    this->markDirty();
}

void WrappedStats::load()
{
    QFile file(statsPath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    this->since_ =
        QDate::fromString(root.value("since").toString(), Qt::ISODate);
    const auto urls = root.value("emoteUrls").toObject();
    for (auto it = urls.begin(); it != urls.end(); ++it)
    {
        this->emoteUrls_.insert(it.key(), it.value().toString());
    }
    const auto days = root.value("days").toObject();
    const auto oldest = QDate::currentDate().addDays(-KEEP_DAYS);
    for (auto it = days.begin(); it != days.end(); ++it)
    {
        const auto date = QDate::fromString(it.key(), Qt::ISODate);
        if (!date.isValid() || date < oldest)
        {
            continue;
        }
        const auto object = it.value().toObject();
        Day day;
        day.messages = fromJson(object.value("m"));
        day.minutes = fromJson(object.value("t"));
        day.emotes = fromJson(object.value("e"));
        day.mentionedBy = fromJson(object.value("mb"));
        day.talkedTo = fromJson(object.value("to"));
        day.mentions = object.value("x").toInt();
        const auto hours = object.value("h").toArray();
        for (int hour = 0; hour < 24 && hour < hours.size(); ++hour)
        {
            day.hours[size_t(hour)] = hours.at(hour).toInt();
        }
        this->days_[date] = std::move(day);
    }
}

void WrappedStats::save()
{
    if (!this->dirty_)
    {
        return;
    }
    const auto oldest = QDate::currentDate().addDays(-KEEP_DAYS);
    QJsonObject days;
    for (auto it = this->days_.begin(); it != this->days_.end();)
    {
        if (it->first < oldest)
        {
            it = this->days_.erase(it);
            continue;
        }
        const auto &day = it->second;
        QJsonArray hours;
        for (const auto count : day.hours)
        {
            hours.append(count);
        }
        days.insert(it->first.toString(Qt::ISODate),
                    QJsonObject{{"m", toJson(day.messages)},
                                {"t", toJson(day.minutes)},
                                {"e", toJson(day.emotes)},
                                {"mb", toJson(day.mentionedBy)},
                                {"to", toJson(day.talkedTo)},
                                {"x", day.mentions},
                                {"h", hours}});
        ++it;
    }
    QSaveFile file(statsPath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    QJsonObject urls;
    for (auto it = this->emoteUrls_.begin(); it != this->emoteUrls_.end(); ++it)
    {
        urls.insert(it.key(), it.value());
    }
    file.write(
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"since", this->since_.toString(Qt::ISODate)},
                                  {"days", days},
                                  {"emoteUrls", urls}})
            .toJson(QJsonDocument::Compact));
    if (file.commit())
    {
        this->dirty_ = false;
    }
}

WrappedStats::Summary WrappedStats::summarize(QDate from, QDate to) const
{
    Summary summary;
    summary.from = from;
    summary.to = to;
    summary.countingSince = this->since_;

    QHash<QString, int> channels;
    QHash<QString, int> watched;
    QHash<QString, int> emotes;
    QHash<QString, int> fans;
    QHash<QString, int> talkedTo;

    int streak = 0;
    QDate previous;
    for (auto it = this->days_.lower_bound(from);
         it != this->days_.end() && it->first <= to; ++it)
    {
        const auto &day = it->second;
        int sent = 0;
        for (auto c = day.messages.begin(); c != day.messages.end(); ++c)
        {
            sent += c.value();
            channels[c.key()] += c.value();
        }
        for (auto c = day.minutes.begin(); c != day.minutes.end(); ++c)
        {
            summary.minutes += c.value();
            watched[c.key()] += c.value();
        }
        for (auto c = day.emotes.begin(); c != day.emotes.end(); ++c)
        {
            emotes[c.key()] += c.value();
        }
        for (auto c = day.mentionedBy.begin(); c != day.mentionedBy.end(); ++c)
        {
            fans[c.key()] += c.value();
        }
        for (auto c = day.talkedTo.begin(); c != day.talkedTo.end(); ++c)
        {
            talkedTo[c.key()] += c.value();
        }
        for (int hour = 0; hour < 24; ++hour)
        {
            summary.hours[size_t(hour)] += day.hours[size_t(hour)];
        }
        summary.mentions += day.mentions;
        summary.messages += sent;

        if (sent > 0)
        {
            summary.activeDays += 1;
            streak = previous.isValid() && previous.addDays(1) == it->first
                         ? streak + 1
                         : 1;
            previous = it->first;
            summary.longestStreak = std::max(summary.longestStreak, streak);
            if (sent > summary.busiestDayMessages)
            {
                summary.busiestDayMessages = sent;
                summary.busiestDay = it->first;
            }
        }
    }

    // Days counted so far in this range, not days before counting began
    QDate countFrom = from;
    if (this->since_.isValid() && this->since_ > countFrom)
    {
        countFrom = this->since_;
    }
    const auto lastDay = std::min(to, QDate::currentDate());
    summary.daysInRange =
        countFrom <= lastDay ? int(countFrom.daysTo(lastDay)) + 1 : 0;

    summary.channels = top(channels, 5);
    summary.watched = top(watched, 5);
    summary.emotes = top(emotes, 5);
    summary.fans = top(fans, 3);
    summary.talkedTo = top(talkedTo, 3);
    return summary;
}

}  // namespace chatterino::wrapped
