// Only built with CHATTERINO_THEME_VIDEO (needs libvpx, libwebm and libyuv).
#pragma once

#include "singletons/ThemeVideoCancellation.hpp"

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QStringList>

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
                                 ThemeVideoStopToken cancellation = {});

ThemeWallpaperData prepareThemeVideo(const QString &source,
                                     ThemeVideoStopToken cancellation = {},
                                     int seconds = 0);

}  // namespace chatterino
