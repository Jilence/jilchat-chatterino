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

/// Record the channel name of the split that owns the active voice message, so
/// the "voice message playing" panel only shows on that channel's tab.
void setActiveVoiceOwner(const QString &channelName);
QString activeVoiceOwner();

/// Whether the given voice message is actively playing (not paused/stopped).
bool isVoicePlaying(const QString &voiceId);

/// Toggle playback: pause if playing, resume if paused, start if not loaded.
void toggleVoiceMessage(const QString &voiceId);

}  // namespace chatterino::jilchat
