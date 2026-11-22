#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QNetworkAccessManager>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

/// Anonymous Twitch chat (justinfan over plain IRC) with Twitch and 7TV
/// emotes, rendered to StyledText. `flood` adds synthetic messages on top so
/// the view is under load even on a quiet channel.
class TwitchChat : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool flood READ flood WRITE setFlood NOTIFY floodChanged)
    Q_PROPERTY(int floodRate READ floodRate WRITE setFloodRate NOTIFY floodChanged)
    Q_PROPERTY(QVariantList emotes READ emotes NOTIFY emotesChanged)
    Q_PROPERTY(double rate READ rate NOTIFY rateChanged)

public:
    enum Role { NameRole = Qt::UserRole + 1, NameColorRole, HtmlRole, SystemRole };

    explicit TwitchChat(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString channel() const { return channel_; }
    void setChannel(const QString &channel);
    bool connected() const { return connected_; }
    bool flood() const { return flood_; }
    void setFlood(bool on);
    int floodRate() const { return floodRate_; }
    void setFloodRate(int perSecond);
    QVariantList emotes() const { return emoteList_; }
    double rate() const { return rate_; }

    Q_INVOKABLE void sendLocal(const QString &text);

signals:
    void channelChanged();
    void connectedChanged();
    void floodChanged();
    void emotesChanged();
    void rateChanged();

private:
    struct Message {
        QString name;
        QString nameColor;
        QString html;
        bool system = false;
    };
    struct Emote {
        QString url;
        int width = 28;
        int height = 28;
    };

    void connectIrc();
    void scheduleReconnect();
    void onReadyRead();
    void handleLine(const QString &line);
    void setConnected(bool on);
    void append(Message message);
    void addSystem(const QString &text);
    void floodTick();
    void loadSevenTv(const QUrl &url, bool global);
    void rebuildEmoteList();

    QString renderText(const QString &text, const QString &emotesTag) const;
    QString renderWords(const QString &segment) const;
    const Emote *findEmote(const QString &word) const;

    QList<Message> messages_;
    QString channel_;
    QString roomId_;
    bool connected_ = false;
    bool retryPending_ = false;

    QTcpSocket socket_;
    QByteArray buffer_;
    QNetworkAccessManager nam_;

    QHash<QString, Emote> builtin_;
    QHash<QString, Emote> globalEmotes_;
    QHash<QString, Emote> channelEmotes_;
    QVariantList emoteList_;
    QStringList emoteNames_;

    bool flood_ = false;
    int floodRate_ = 40;
    double floodAccum_ = 0;
    QTimer floodTimer_;

    QTimer rateTimer_;
    int counted_ = 0;
    double rate_ = 0;
};
