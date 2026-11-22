#include "TwitchChat.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRandomGenerator>

#include <algorithm>

namespace {

constexpr int kMaxMessages = 500;
constexpr int kEmoteHeight = 26;

const char *const kTwitchColors[] = {
    "#FF0000", "#0000FF", "#008000", "#B22222", "#FF7F50",
    "#9ACD32", "#FF4500", "#2E8B57", "#DAA520", "#D2691E",
    "#5F9EA0", "#1E90FF", "#FF69B4", "#8A2BE2", "#00FF7F",
};

struct Builtin {
    const char *name;
    const char *id;
};
const Builtin kTwitchEmotes[] = {
    {"Kappa", "25"},          {"LUL", "425618"},          {"PogChamp", "305954156"},
    {"BibleThump", "86"},     {"Kreygasm", "41"},         {"NotLikeThis", "58765"},
    {"ResidentSleeper", "245"}, {"SeemsGood", "64138"},   {"TriHard", "120232"},
    {"4Head", "354"},         {"DansGame", "33"},         {"HeyGuys", "30259"},
    {"VoHiYo", "81274"},      {"WutFace", "28087"},       {"CoolStoryBob", "123171"},
};

const char *const kNames[] = {
    "leafy_fan", "Jilence", "xXgamerXx", "kekw_enjoyer", "chatter420", "NightOwl",
    "pixelpanda", "HypeTrain", "zuschauer99", "modmaster", "lurker_supreme",
    "CaptainChat", "emote_spammer", "TwitchUser", "nordlicht", "Brotkasten",
};

const char *const kWords[] = {
    "lol", "W", "L", "true", "no shot", "chat is this real", "bro", "insane", "clip it",
    "first", "?", "LETSGO", "GG", "hä", "digga", "omg", "sheesh", "wtf", "real",
    "fr fr", "nah", "aint no way", "streamer won", "what", "ok", "hahaha", "ratio",
    "W stream", "voll gut", "geil", "sus", "LMAO", "wie bitte", "same", "+1", "clean",
};

QString twitchEmoteUrl(const QString &id)
{
    return QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/static/dark/2.0").arg(id);
}

QString imgTag(const QString &url, int width, int height)
{
    const int w = height > 0 ? qRound(double(width) * kEmoteHeight / height) : kEmoteHeight;
    return QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\" align=\"middle\">")
        .arg(url)
        .arg(w)
        .arg(kEmoteHeight);
}

/// Lightens dark name colours so they stay readable on the dark chat.
QString readableColor(const QString &raw, const QString &name)
{
    QColor c(raw);
    if (!c.isValid())
        c = QColor(kTwitchColors[qHash(name.toLower()) % std::size(kTwitchColors)]);
    float h, s, l, a;
    c.getHslF(&h, &s, &l, &a);
    if (l < 0.6f)
        l = 0.6f + (l * 0.15f);
    c.setHslF(h, std::min(s, 0.9f), l, 1.0f);
    return c.name();
}

QString unescapeTag(QString v)
{
    v.replace(QStringLiteral("\\s"), QStringLiteral(" "));
    v.replace(QStringLiteral("\\:"), QStringLiteral(";"));
    v.replace(QStringLiteral("\\r"), QString());
    v.replace(QStringLiteral("\\n"), QString());
    v.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    return v;
}

int rnd(int bound)
{
    return int(QRandomGenerator::global()->bounded(bound));
}

}  // namespace

TwitchChat::TwitchChat(QObject *parent)
    : QAbstractListModel(parent)
{
    for (const auto &e : kTwitchEmotes)
        builtin_.insert(QString::fromLatin1(e.name), Emote{twitchEmoteUrl(QString::fromLatin1(e.id)), 28, 28});

    connect(&socket_, &QTcpSocket::connected, this, [this] {
        const auto nick = QStringLiteral("justinfan%1").arg(10000 + rnd(80000));
        socket_.write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
        socket_.write("PASS SCHMOOPIIE\r\n");
        socket_.write(QStringLiteral("NICK %1\r\n").arg(nick).toUtf8());
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &TwitchChat::onReadyRead);
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        setConnected(false);
        scheduleReconnect();
    });
    connect(&socket_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        addSystem(QStringLiteral("Verbindungsfehler: %1").arg(socket_.errorString().toHtmlEscaped()));
        setConnected(false);
        scheduleReconnect();
    });

    floodTimer_.setInterval(16);
    connect(&floodTimer_, &QTimer::timeout, this, &TwitchChat::floodTick);

    rateTimer_.setInterval(1000);
    connect(&rateTimer_, &QTimer::timeout, this, [this] {
        rate_ = counted_;
        counted_ = 0;
        emit rateChanged();
    });
    rateTimer_.start();

    QTimer::singleShot(0, this, &TwitchChat::connectIrc);
    loadSevenTv(QUrl(QStringLiteral("https://7tv.io/v3/emote-sets/global")), true);
}

int TwitchChat::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(messages_.size());
}

QVariant TwitchChat::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= messages_.size())
        return {};
    const auto &m = messages_.at(index.row());
    switch (role) {
    case NameRole:
        return m.name;
    case NameColorRole:
        return m.nameColor;
    case HtmlRole:
        return m.html;
    case SystemRole:
        return m.system;
    }
    return {};
}

QHash<int, QByteArray> TwitchChat::roleNames() const
{
    return {{NameRole, "name"}, {NameColorRole, "nameColor"}, {HtmlRole, "html"}, {SystemRole, "system"}};
}

void TwitchChat::setChannel(const QString &channel)
{
    const auto c = channel.trimmed().toLower();
    if (c == channel_)
        return;
    const auto old = channel_;
    channel_ = c;
    emit channelChanged();

    beginResetModel();
    messages_.clear();
    endResetModel();
    channelEmotes_.clear();
    roomId_.clear();
    rebuildEmoteList();

    if (connected_) {
        if (!old.isEmpty())
            socket_.write(QStringLiteral("PART #%1\r\n").arg(old).toUtf8());
        socket_.write(QStringLiteral("JOIN #%1\r\n").arg(channel_).toUtf8());
    }
    addSystem(QStringLiteral("Betrete #%1 …").arg(channel_.toHtmlEscaped()));
}

void TwitchChat::setFlood(bool on)
{
    if (on == flood_)
        return;
    flood_ = on;
    floodAccum_ = 0;
    on ? floodTimer_.start() : floodTimer_.stop();
    emit floodChanged();
}

void TwitchChat::setFloodRate(int perSecond)
{
    perSecond = std::clamp(perSecond, 1, 1000);
    if (perSecond == floodRate_)
        return;
    floodRate_ = perSecond;
    emit floodChanged();
}

void TwitchChat::sendLocal(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    append({QStringLiteral("du"), QStringLiteral("#c4b5ff"), renderText(text, {}), false});
}

void TwitchChat::connectIrc()
{
    retryPending_ = false;
    socket_.abort();
    buffer_.clear();
    socket_.connectToHost(QStringLiteral("irc.chat.twitch.tv"), 6667);
}

void TwitchChat::scheduleReconnect()
{
    if (retryPending_)
        return;
    retryPending_ = true;
    QTimer::singleShot(3000, this, &TwitchChat::connectIrc);
}

void TwitchChat::setConnected(bool on)
{
    if (on == connected_)
        return;
    connected_ = on;
    emit connectedChanged();
}

void TwitchChat::onReadyRead()
{
    buffer_ += socket_.readAll();
    qsizetype end;
    while ((end = buffer_.indexOf("\r\n")) >= 0) {
        const auto line = QString::fromUtf8(buffer_.left(end));
        buffer_.remove(0, end + 2);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void TwitchChat::handleLine(const QString &raw)
{
    QString line = raw;
    QHash<QString, QString> tags;
    if (line.startsWith('@')) {
        const auto sp = line.indexOf(' ');
        for (const auto &kv : QStringView(line).mid(1, sp - 1).split(';')) {
            const auto eq = kv.indexOf('=');
            if (eq > 0)
                tags.insert(kv.left(eq).toString(), unescapeTag(kv.mid(eq + 1).toString()));
        }
        line = line.mid(sp + 1);
    }
    QString prefix;
    if (line.startsWith(':')) {
        const auto sp = line.indexOf(' ');
        prefix = line.mid(1, sp - 1);
        line = line.mid(sp + 1);
    }
    QString trailing;
    const auto ti = line.indexOf(QStringLiteral(" :"));
    if (ti >= 0) {
        trailing = line.mid(ti + 2);
        line = line.left(ti);
    }
    const auto params = line.split(' ', Qt::SkipEmptyParts);
    const auto cmd = params.value(0);
    const auto target = params.value(1);

    if (cmd == QLatin1String("PING")) {
        socket_.write(QStringLiteral("PONG :%1\r\n").arg(trailing).toUtf8());
    } else if (cmd == QLatin1String("001")) {
        setConnected(true);
        if (!channel_.isEmpty())
            socket_.write(QStringLiteral("JOIN #%1\r\n").arg(channel_).toUtf8());
    } else if (cmd == QLatin1String("JOIN")) {
        if (target == '#' + channel_)
            addSystem(QStringLiteral("Verbunden mit #%1").arg(channel_.toHtmlEscaped()));
    } else if (cmd == QLatin1String("ROOMSTATE") || cmd == QLatin1String("PRIVMSG")) {
        if (target != '#' + channel_)
            return;
        const auto room = tags.value(QStringLiteral("room-id"));
        if (!room.isEmpty() && room != roomId_) {
            roomId_ = room;
            loadSevenTv(QUrl(QStringLiteral("https://7tv.io/v3/users/twitch/%1").arg(room)), false);
        }
        if (cmd == QLatin1String("ROOMSTATE"))
            return;

        auto name = tags.value(QStringLiteral("display-name"));
        if (name.isEmpty())
            name = prefix.section('!', 0, 0);
        const auto color = readableColor(tags.value(QStringLiteral("color")), name);
        QString text = trailing;
        bool action = false;
        if (text.startsWith(QStringLiteral("\x01" "ACTION ")) && text.endsWith('\x01')) {
            text = text.mid(8, text.size() - 9);
            action = true;
        }
        auto html = renderText(text, tags.value(QStringLiteral("emotes")));
        if (action)
            html = QStringLiteral("<font color=\"%1\"><i>%2</i></font>").arg(color, html);
        append({name.toHtmlEscaped(), color, html, false});
    } else if (cmd == QLatin1String("USERNOTICE")) {
        if (target != '#' + channel_)
            return;
        auto msg = tags.value(QStringLiteral("system-msg")).toHtmlEscaped();
        if (!trailing.isEmpty())
            msg += QStringLiteral(" — ") + renderText(trailing, tags.value(QStringLiteral("emotes")));
        addSystem(msg);
    }
}

void TwitchChat::append(Message message)
{
    // Drop one at a time: removing a big chunk relayouts the whole view at once.
    if (messages_.size() >= kMaxMessages) {
        beginRemoveRows({}, 0, 0);
        messages_.removeFirst();
        endRemoveRows();
    }
    const int row = int(messages_.size());
    beginInsertRows({}, row, row);
    messages_.append(std::move(message));
    endInsertRows();
    ++counted_;
}

void TwitchChat::addSystem(const QString &text)
{
    append({{}, {}, text, true});
}

void TwitchChat::floodTick()
{
    floodAccum_ += floodRate_ * (floodTimer_.interval() / 1000.0);
    while (floodAccum_ >= 1.0) {
        floodAccum_ -= 1.0;
        const QString name = QString::fromLatin1(kNames[rnd(int(std::size(kNames)))]) +
                             (rnd(3) == 0 ? QString::number(rnd(999)) : QString());
        QStringList tokens;
        const int n = 1 + rnd(11);
        for (int i = 0; i < n; ++i) {
            if (!emoteNames_.isEmpty() && rnd(10) < 4)
                tokens << emoteNames_.at(rnd(int(emoteNames_.size())));
            else
                tokens << QString::fromUtf8(kWords[rnd(int(std::size(kWords)))]);
        }
        const auto color = QColor::fromHslF(rnd(360) / 360.0f, 0.75f, 0.68f).name();
        append({name, color, renderText(tokens.join(' '), {}), false});
    }
}

void TwitchChat::loadSevenTv(const QUrl &url, bool global)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("leafyrino-liquid-glass-prototype"));
    auto *reply = nam_.get(req);
    const auto forRoom = roomId_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, global, forRoom] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        if (!global && forRoom != roomId_)
            return;
        const auto root = QJsonDocument::fromJson(reply->readAll()).object();
        const auto list = global ? root.value(QStringLiteral("emotes")).toArray()
                                 : root.value(QStringLiteral("emote_set")).toObject().value(QStringLiteral("emotes")).toArray();
        auto &target = global ? globalEmotes_ : channelEmotes_;
        for (const auto &v : list) {
            const auto e = v.toObject();
            const auto host = e.value(QStringLiteral("data")).toObject().value(QStringLiteral("host")).toObject();
            const auto base = QStringLiteral("https:") + host.value(QStringLiteral("url")).toString();
            QJsonObject file;
            for (const auto &f : host.value(QStringLiteral("files")).toArray()) {
                const auto fo = f.toObject();
                if (fo.value(QStringLiteral("name")).toString() == QLatin1String("2x.webp")) {
                    file = fo;
                    break;
                }
            }
            if (file.isEmpty())
                continue;
            target.insert(e.value(QStringLiteral("name")).toString(),
                          Emote{base + QStringLiteral("/2x.webp"), file.value(QStringLiteral("width")).toInt(56),
                                file.value(QStringLiteral("height")).toInt(56)});
        }
        rebuildEmoteList();
        if (!global)
            addSystem(QStringLiteral("%1 7TV-Emotes geladen").arg(target.size()));
    });
}

void TwitchChat::rebuildEmoteList()
{
    emoteList_.clear();
    emoteNames_.clear();
    auto add = [this](const QHash<QString, Emote> &set) {
        QStringList names = set.keys();
        names.sort(Qt::CaseInsensitive);
        for (const auto &n : names) {
            const auto &e = set[n];
            emoteList_.append(QVariantMap{{"name", n}, {"url", e.url}, {"width", e.width}, {"height", e.height}});
            emoteNames_ << n;
        }
    };
    add(channelEmotes_);
    add(globalEmotes_);
    for (auto it = builtin_.cbegin(); it != builtin_.cend(); ++it)
        emoteNames_ << it.key();
    emit emotesChanged();
}

const TwitchChat::Emote *TwitchChat::findEmote(const QString &word) const
{
    if (auto it = channelEmotes_.constFind(word); it != channelEmotes_.cend())
        return &*it;
    if (auto it = globalEmotes_.constFind(word); it != globalEmotes_.cend())
        return &*it;
    if (auto it = builtin_.constFind(word); it != builtin_.cend())
        return &*it;
    return nullptr;
}

QString TwitchChat::renderWords(const QString &segment) const
{
    QStringList words = segment.split(' ');
    for (auto &w : words) {
        if (const auto *e = findEmote(w))
            w = imgTag(e->url, e->width, e->height);
        else
            w = w.toHtmlEscaped();
    }
    return words.join(' ');
}

QString TwitchChat::renderText(const QString &text, const QString &emotesTag) const
{
    struct Range {
        qsizetype start;
        qsizetype end;
        QString id;
    };
    QList<Range> ranges;
    if (!emotesTag.isEmpty()) {
        for (const auto &part : emotesTag.split('/')) {
            const auto id = part.section(':', 0, 0);
            for (const auto &r : part.section(':', 1).split(',')) {
                bool okA = false, okB = false;
                const auto a = r.section('-', 0, 0).toLongLong(&okA);
                const auto b = r.section('-', 1, 1).toLongLong(&okB);
                if (okA && okB)
                    ranges.append({a, b, id});
            }
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range &x, const Range &y) { return x.start < y.start; });
    }
    if (ranges.isEmpty())
        return renderWords(text);

    // Twitch counts emote positions in code points, not UTF-16 units.
    const QList<uint> cps = text.toUcs4();
    const auto slice = [&](qsizetype from, qsizetype len) {
        return QString::fromUcs4(reinterpret_cast<const char32_t *>(cps.constData() + from), len);
    };
    QString out;
    qsizetype pos = 0;
    for (const auto &r : ranges) {
        if (r.start < pos || r.end >= cps.size())
            continue;
        out += renderWords(slice(pos, r.start - pos));
        out += imgTag(twitchEmoteUrl(r.id), 28, 28);
        pos = r.end + 1;
    }
    out += renderWords(slice(pos, cps.size() - pos));
    return out;
}
