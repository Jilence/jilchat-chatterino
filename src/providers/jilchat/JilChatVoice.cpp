// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jilchat/JilChatVoice.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <functional>

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
#    include <QAudioOutput>
#    include <QHash>
#    include <QList>
#    include <QMediaPlayer>
#    include <QObject>
#    include <QPointer>
#endif

namespace chatterino::jilchat {

namespace {

QString jilChatJwt;

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
QHash<QString, QList<QPointer<QAudioOutput>>> activeAudioOutputs;
QHash<QString, QPointer<QMediaPlayer>> activePlayers;
QHash<QString, qint64> activePositions;
QHash<QString, qint64> activeDurations;
#endif

QJsonObject volumeOverrides()
{
    const auto document = QJsonDocument::fromJson(
        getSettings()->jilChatVoiceVolumeOverrides.getValue().toUtf8());
    if (!document.isObject())
    {
        return {};
    }
    return document.object();
}

void saveVolumeOverrides(const QJsonObject &overrides)
{
    getSettings()->jilChatVoiceVolumeOverrides =
        QString::fromUtf8(QJsonDocument(overrides).toJson(
            QJsonDocument::Compact));
}

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
void updateActiveVoiceVolume(const QString &voiceId, int volume)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    runInGuiThread([voiceId, volume] {
        auto it = activeAudioOutputs.find(voiceId);
        if (it == activeAudioOutputs.end())
        {
            return;
        }

        for (auto outputIt = it->begin(); outputIt != it->end();)
        {
            auto *output = outputIt->data();
            if (output == nullptr)
            {
                outputIt = it->erase(outputIt);
                continue;
            }

            output->setVolume(std::clamp(volume, 0, 100) / 100.F);
            ++outputIt;
        }

        if (it->isEmpty())
        {
            activeAudioOutputs.erase(it);
        }
    });
}
#endif

void requestVoiceRepaint()
{
    getApp()->getWindows()->repaintGifEmotes();
}

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
void applyStartPosition(QMediaPlayer *player, double startProgress)
{
    if (startProgress < 0)
    {
        return;
    }

    const auto duration = player->duration();
    if (duration <= 0)
    {
        return;
    }

    player->setPosition(
        static_cast<qint64>(duration * std::clamp(startProgress, 0.0, 1.0)));
}
#endif

void playVoiceFile(const QString &path, const QString &voiceId,
                   double startProgress = -1.0)
{
#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    runInGuiThread([path, voiceId, startProgress] {
        if (auto existing = activePlayers.value(voiceId); existing != nullptr)
        {
            existing->deleteLater();
        }

        auto *player = new QMediaPlayer;
        auto *audioOutput = new QAudioOutput(player);

        audioOutput->setVolume(getVoiceVolume(voiceId) / 100.F);
        player->setAudioOutput(audioOutput);
        player->setSource(QUrl::fromLocalFile(path));
        activePlayers[voiceId] = QPointer<QMediaPlayer>(player);
        activeAudioOutputs[voiceId].append(QPointer<QAudioOutput>(audioOutput));

        QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player,
                         [player, voiceId,
                          startProgress](QMediaPlayer::MediaStatus status) {
                             if (status == QMediaPlayer::LoadedMedia ||
                                 status == QMediaPlayer::BufferedMedia)
                             {
                                 applyStartPosition(player, startProgress);
                             }
                             if (status == QMediaPlayer::EndOfMedia ||
                                 status == QMediaPlayer::InvalidMedia)
                             {
                                 player->deleteLater();
                             }
                         });
        QObject::connect(player, &QMediaPlayer::positionChanged, player,
                         [voiceId](qint64 position) {
                             activePositions[voiceId] = position;
                             requestVoiceRepaint();
                         });
        QObject::connect(player, &QMediaPlayer::durationChanged, player,
                         [voiceId](qint64 duration) {
                             activeDurations[voiceId] = duration;
                             requestVoiceRepaint();
                         });
        QObject::connect(player, &QMediaPlayer::errorOccurred, player,
                         [player, voiceId](QMediaPlayer::Error /*error*/,
                                  const QString & /*errorString*/) {
                             QDesktopServices::openUrl(QUrl(
                                 QStringLiteral("https://jil.chat/v/%1")
                                     .arg(voiceId)));
                             player->deleteLater();
                         });
        QObject::connect(player, &QObject::destroyed,
                         [voiceId, playerPtr = QPointer<QMediaPlayer>(player),
                          output = QPointer<QAudioOutput>(audioOutput)] {
                             const auto activePlayer =
                                 activePlayers.value(voiceId);
                             const bool destroyedActivePlayer =
                                 activePlayer == playerPtr ||
                                 activePlayer == nullptr;
                             if (destroyedActivePlayer)
                             {
                                 activePlayers.remove(voiceId);
                                 activePositions.remove(voiceId);
                                 activeDurations.remove(voiceId);
                             }
                             auto it = activeAudioOutputs.find(voiceId);
                             if (it == activeAudioOutputs.end())
                             {
                                 return;
                             }
                             it->removeAll(output);
                             if (it->isEmpty())
                             {
                                 activeAudioOutputs.erase(it);
                             }
                             requestVoiceRepaint();
                         });

        player->play();
    });
#else
    (void)path;
    (void)startProgress;
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://jil.chat/v/%1").arg(voiceId)));
#endif
}

QString cachePathFor(const QString &voiceId, const QUrl &audioUrl)
{
    auto cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty())
    {
        cacheRoot = QDir::tempPath();
    }

    QDir dir(cacheRoot);
    dir.mkpath(QStringLiteral("jilchat-voice"));
    dir.cd(QStringLiteral("jilchat-voice"));

    auto suffix = QFileInfo(audioUrl.path()).suffix().toLower();
    if (suffix.isEmpty() || suffix == QStringLiteral("mp3"))
    {
        suffix = QStringLiteral("mp4");
    }

    const auto safeId =
        QString::fromLatin1(QCryptographicHash::hash(voiceId.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                .toHex());
    return dir.filePath(safeId + QStringLiteral(".") + suffix);
}

void playDownloadedVoice(const QString &voiceId, const QUrl &audioUrl,
                         double startProgress = -1.0)
{
    const auto path = cachePathFor(voiceId, audioUrl);
    if (QFileInfo::exists(path))
    {
        playVoiceFile(path, voiceId, startProgress);
        return;
    }

    NetworkRequest(audioUrl)
        .timeout(15 * 1000)
        .followRedirects(true)
        .onSuccess([path, voiceId,
                    startProgress](const NetworkResult &result) -> Outcome {
            const auto tempPath =
                path + QStringLiteral(".") +
                QUuid::createUuid().toString(QUuid::Id128) +
                QStringLiteral(".tmp");
            QFile file(tempPath);
            if (!file.open(QIODevice::WriteOnly))
            {
                return Failure;
            }
            file.write(result.getData());
            file.close();
            if (!QFile::rename(tempPath, path))
            {
                if (QFileInfo::exists(path))
                {
                    QFile::remove(tempPath);
                    playVoiceFile(path, voiceId, startProgress);
                    return Success;
                }
                QFile::remove(tempPath);
                return Failure;
            }

            playVoiceFile(path, voiceId, startProgress);
            return Success;
        })
        .execute();
}

void authenticateJilChat(const std::function<void(QString)> &onSuccess,
                         const std::function<void()> &onFailure)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon() || account->getOAuthToken().isEmpty())
    {
        onFailure();
        return;
    }

    QJsonObject body;
    body["twitch_token"] = account->getOAuthToken();

    NetworkRequest(QUrl(QStringLiteral("https://api.jil.chat/v1/auth/login")),
                   NetworkRequestType::Post)
        .timeout(10 * 1000)
        .followRedirects(true)
        .json(body)
        .onSuccess([onSuccess, onFailure](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto jwt = root.value("jwt").toString();
            if (jwt.isEmpty())
            {
                onFailure();
                return;
            }

            jilChatJwt = jwt;
            onSuccess(jwt);
        })
        .onError([onFailure](const NetworkResult &) {
            onFailure();
        })
        .execute();
}

void fetchVoiceMeta(const QString &voiceId, const QString &jwt,
                    bool retriedAuth = false, double startProgress = -1.0)
{
    const QUrl metaUrl(QStringLiteral("https://api.jil.chat/v1/voice/%1")
                           .arg(voiceId));
    auto request = NetworkRequest(metaUrl)
                       .timeout(10 * 1000)
                       .followRedirects(true)
                       .header("Accept", "application/json");
    if (!jwt.isEmpty())
    {
        request = std::move(request).header(
            "Authorization", QStringLiteral("Bearer %1").arg(jwt));
    }

    std::move(request)
        .onSuccess([voiceId,
                    startProgress](const NetworkResult &result) -> Outcome {
            const auto root = result.parseJson();
            const auto audioUrlString = root.value("audio_url").toString();
            const QUrl audioUrl(audioUrlString);
            if (!audioUrl.isValid() || audioUrl.isEmpty())
            {
                QDesktopServices::openUrl(QUrl(
                    QStringLiteral("https://jil.chat/v/%1").arg(voiceId)));
                return Failure;
            }

            playDownloadedVoice(voiceId, audioUrl, startProgress);
            return Success;
        })
        .onError([voiceId, retriedAuth,
                  startProgress](const NetworkResult &result) {
            const auto status = result.status().value_or(0);
            if ((status == 401 || status == 403) && !retriedAuth)
            {
                jilChatJwt.clear();
                authenticateJilChat(
                    [voiceId, startProgress](const QString &freshJwt) {
                        fetchVoiceMeta(voiceId, freshJwt, true,
                                       startProgress);
                    },
                    [voiceId] {
                        QDesktopServices::openUrl(QUrl(
                            QStringLiteral("https://jil.chat/v/%1")
                                .arg(voiceId)));
                    });
                return;
            }

            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://jil.chat/v/%1").arg(voiceId)));
        })
        .execute();
}

}  // namespace

int getVoiceVolume(const QString &voiceId)
{
    const auto fallback =
        std::clamp(getSettings()->jilChatVoiceVolume.getValue(), 0, 100);
    if (voiceId.isEmpty())
    {
        return fallback;
    }

    const auto overrides = volumeOverrides();
    const auto value = overrides.value(voiceId);
    if (!value.isDouble())
    {
        return fallback;
    }

    return std::clamp(value.toInt(fallback), 0, 100);
}

void setVoiceVolume(const QString &voiceId, int volume)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    auto overrides = volumeOverrides();
    overrides[voiceId] = std::clamp(volume, 0, 100);
    saveVolumeOverrides(overrides);
#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    updateActiveVoiceVolume(voiceId, getVoiceVolume(voiceId));
#endif
}

void resetVoiceVolume(const QString &voiceId)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    auto overrides = volumeOverrides();
    overrides.remove(voiceId);
    saveVolumeOverrides(overrides);
#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    updateActiveVoiceVolume(voiceId, getVoiceVolume(voiceId));
#endif
}

QString getActiveVoiceId()
{
#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    for (auto it = activePlayers.cbegin(); it != activePlayers.cend(); ++it)
    {
        if (it.value() != nullptr)
        {
            return it.key();
        }
    }
#endif
    return {};
}

double getVoiceProgress(const QString &voiceId)
{
#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    const auto duration = activeDurations.value(voiceId, 0);
    if (duration <= 0)
    {
        return 0.0;
    }

    return std::clamp(activePositions.value(voiceId, 0) /
                          static_cast<double>(duration),
                      0.0, 1.0);
#else
    (void)voiceId;
    return 0.0;
#endif
}

void playVoiceMessage(const QString &voiceId)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    fetchVoiceMeta(voiceId, {});
}

void seekVoiceMessage(const QString &voiceId, double progress)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    progress = std::clamp(progress, 0.0, 1.0);

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    runInGuiThread([voiceId, progress] {
        auto player = activePlayers.value(voiceId);
        if (player == nullptr)
        {
            fetchVoiceMeta(voiceId, {}, false, progress);
            return;
        }

        applyStartPosition(player, progress);
        player->play();
        requestVoiceRepaint();
    });
#else
    (void)progress;
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://jil.chat/v/%1").arg(voiceId)));
#endif
}

void stopVoiceMessage(const QString &voiceId)
{
    if (voiceId.isEmpty())
    {
        return;
    }

#ifdef CHATTERINO_HAVE_QT_MULTIMEDIA
    runInGuiThread([voiceId] {
        auto player = activePlayers.value(voiceId);
        if (player == nullptr)
        {
            return;
        }

        player->stop();
        player->deleteLater();
        requestVoiceRepaint();
    });
#else
    (void)voiceId;
#endif
}

}  // namespace chatterino::jilchat
