// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/jilchat/JilChatVoice.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QAudioOutput>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace chatterino::jilchat {

namespace {

QHash<QString, QList<QPointer<QAudioOutput>>> activeAudioOutputs;

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

void playVoiceFile(const QString &path, const QString &voiceId)
{
    runInGuiThread([path, voiceId] {
        auto *player = new QMediaPlayer;
        auto *audioOutput = new QAudioOutput(player);

        audioOutput->setVolume(getVoiceVolume(voiceId) / 100.F);
        player->setAudioOutput(audioOutput);
        player->setSource(QUrl::fromLocalFile(path));
        activeAudioOutputs[voiceId].append(QPointer<QAudioOutput>(audioOutput));

        QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player,
                         [player](QMediaPlayer::MediaStatus status) {
                             if (status == QMediaPlayer::EndOfMedia ||
                                 status == QMediaPlayer::InvalidMedia)
                             {
                                 player->deleteLater();
                             }
                         });
        QObject::connect(player, &QMediaPlayer::errorOccurred, player,
                         [player](QMediaPlayer::Error /*error*/,
                                  const QString & /*errorString*/) {
                             player->deleteLater();
                         });
        QObject::connect(player, &QObject::destroyed,
                         [voiceId, output = QPointer<QAudioOutput>(
                                       audioOutput)] {
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
                         });

        player->play();
    });
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

    auto suffix = QFileInfo(audioUrl.path()).suffix();
    if (suffix.isEmpty())
    {
        suffix = QStringLiteral("mp3");
    }

    const auto safeId =
        QString::fromLatin1(QCryptographicHash::hash(voiceId.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                .toHex());
    return dir.filePath(safeId + QStringLiteral(".") + suffix);
}

void playDownloadedVoice(const QString &voiceId, const QUrl &audioUrl)
{
    const auto path = cachePathFor(voiceId, audioUrl);
    if (QFileInfo::exists(path))
    {
        playVoiceFile(path, voiceId);
        return;
    }

    NetworkRequest(audioUrl)
        .timeout(15 * 1000)
        .followRedirects(true)
        .onSuccess([path, voiceId](const NetworkResult &result) -> Outcome {
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
                    playVoiceFile(path, voiceId);
                    return Success;
                }
                QFile::remove(tempPath);
                return Failure;
            }

            playVoiceFile(path, voiceId);
            return Success;
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
    updateActiveVoiceVolume(voiceId, getVoiceVolume(voiceId));
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
    updateActiveVoiceVolume(voiceId, getVoiceVolume(voiceId));
}

void playVoiceMessage(const QString &voiceId)
{
    if (voiceId.isEmpty())
    {
        return;
    }

    const QUrl metaUrl(QStringLiteral("https://api.jil.chat/v1/voice/%1")
                           .arg(voiceId));
    NetworkRequest(metaUrl)
        .timeout(10 * 1000)
        .followRedirects(true)
        .onSuccess([voiceId](const NetworkResult &result) -> Outcome {
            const auto root = result.parseJson();
            const auto audioUrlString = root.value("audio_url").toString();
            const QUrl audioUrl(audioUrlString);
            if (!audioUrl.isValid() || audioUrl.isEmpty())
            {
                return Failure;
            }

            playDownloadedVoice(voiceId, audioUrl);
            return Success;
        })
        .execute();
}

}  // namespace chatterino::jilchat
