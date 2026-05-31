// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QUrl>

namespace chatterino::jilchat {

int getVoiceVolume(const QString &voiceId);
void setVoiceVolume(const QString &voiceId, int volume);
void resetVoiceVolume(const QString &voiceId);
QString getActiveVoiceId();
double getVoiceProgress(const QString &voiceId);
void playVoiceMessage(const QString &voiceId);
void seekVoiceMessage(const QString &voiceId, double progress);
void stopVoiceMessage(const QString &voiceId);

}  // namespace chatterino::jilchat
