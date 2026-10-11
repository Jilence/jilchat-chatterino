// Only built with CHATTERINO_THEME_VIDEO (needs libvpx, libwebm and libyuv).
#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QStringList>

#include <stop_token>

namespace chatterino {

/// A background ready to be stored: the bytes of a WebM video.
struct ThemeWallpaperData {
    QByteArray bytes;
    QString error;
    QString extension = QStringLiteral("webp");
};

struct ThemeVideoInfo {
    QString error;
    double duration = 0;
    QSize outputSize;
    double framesPerSecond = 24;
};

bool isThemeVideoInput(const QString &source);
QStringList themeVideoInputExtensions();
ThemeVideoInfo inspectThemeVideo(const QString &source,
                                 std::stop_token cancellation = {});

ThemeWallpaperData prepareThemeVideo(const QString &source,
                                     std::stop_token cancellation = {},
                                     int seconds = 0);

}  // namespace chatterino
