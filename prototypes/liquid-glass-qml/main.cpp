#include "FrameStats.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickGraphicsConfiguration>
#include <QStandardPaths>

#include <cstdio>

namespace {

/// Emote images are cached on disk, like in Chatterino.
class CachingNamFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *nam = new QNetworkAccessManager(parent);
        auto *cache = new QNetworkDiskCache(nam);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                                 QStringLiteral("/emotes"));
        cache->setMaximumCacheSize(256 * 1024 * 1024);
        nam->setCache(cache);
        return nam;
    }
};

}  // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Leafyrino Liquid Glass"));
    CachingNamFactory namFactory;

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption channelsOpt({"c", "channels"}, "Comma separated channels.", "list",
                                   "xqc,papaplatte,kaicenat,forsen");
    QCommandLineOption stressOpt("stress", "Start with synthetic message flood and the moving lens.");
    QCommandLineOption noVsyncOpt("novsync", "Render as fast as possible (shows headroom).");
    QCommandLineOption noGlassOpt("noglass", "Start with flat surfaces instead of glass.");
    QCommandLineOption apiOpt("api", "d3d11 | d3d12 | vulkan | opengl", "api");
    QCommandLineOption benchOpt("bench", "Measure for N seconds after a 3 s warmup, print, quit.", "seconds");
    QCommandLineOption shotOpt("screenshot", "Save a PNG of the window at the end of --bench.", "path");
    QCommandLineOption sizeOpt("size", "Window size, e.g. 460x840.", "WxH");
    QCommandLineOption pickerOpt("picker", "Start with the emote picker open.");
    QCommandLineOption prismOpt("prism", "Start with chromatic dispersion on.");
    parser.addOptions({channelsOpt, stressOpt, noVsyncOpt, noGlassOpt, apiOpt, benchOpt, shotOpt, sizeOpt, pickerOpt, prismOpt});
    parser.process(app);

    if (parser.isSet(noVsyncOpt)) {
        auto format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }
    const auto api = parser.value(apiOpt).toLower();
    if (api == "d3d11")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    else if (api == "d3d12")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
    else if (api == "vulkan")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    else if (api == "opengl")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    int width = 460, height = 840;
    if (parser.isSet(sizeOpt)) {
        const auto parts = parser.value(sizeOpt).split('x');
        if (parts.size() == 2) {
            width = parts[0].toInt();
            height = parts[1].toInt();
        }
    }

    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&namFactory);
    engine.rootContext()->setContextProperty(
        "Launch", QVariantMap{
                      {"channels", parser.value(channelsOpt).split(',', Qt::SkipEmptyParts)},
                      {"stress", parser.isSet(stressOpt)},
                      {"glass", !parser.isSet(noGlassOpt)},
                      {"width", width},
                      {"height", height},
                      {"picker", parser.isSet(pickerOpt)},
                      {"prism", parser.isSet(prismOpt)},
                  });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("LiquidGlass", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;
    // GPU timestamps for the HUD; must be set before the window is shown.
    auto config = window->graphicsConfiguration();
    config.setTimestamps(true);
    window->setGraphicsConfiguration(config);
    window->show();

    const int bench = parser.value(benchOpt).toInt();
    if (bench > 0) {
        auto *stats = window->findChild<FrameStats *>();
        const auto shot = parser.value(shotOpt);
        QTimer::singleShot(3000, window, [stats] {
            if (stats)
                stats->resetTotals();
        });
        QTimer::singleShot((3 + bench) * 1000, window, [window, stats, shot] {
            if (stats) {
                std::printf("%s\n", qPrintable(stats->summary()));
                std::fflush(stdout);
            }
            if (!shot.isEmpty())
                window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
