#include "widgets/helper/CanvasChatSurface.hpp"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QDebug>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const bool stress = app.arguments().contains(QStringLiteral("--stress"));
    int frames = 0;
    QWidget window;
    window.resize(420, 240);
    window.setWindowTitle("Canvas Painter smoke test");
    QPixmap row(300, 32);
    row.fill(QColor("#303648"));
    { QPainter text(&row); text.setPen(Qt::white); text.drawText(8, 22, "JilChat: cached message + animated image"); }
    auto draw = [&](QPainter &p) {
        p.fillRect(QRect(0, 0, 420, 240), QColor("#151821"));
        p.drawPixmap(15, 15, row);
        p.save();
        p.setClipRect(QRect(20, 70, 60, 40));
        p.fillRect(QRect(0, 50, 150, 90), QColor("#20c080"));
        p.restore();
        p.setOpacity(0.5);
        p.fillRect(QRect(180, 70, 60, 40), QColor("#20c080"));
        p.setOpacity(1);
        p.setPen(Qt::white);
        p.drawText(QPointF(20, 170), "Qt Canvas Painter / GPU");
    };
    auto *surface = new chatterino::CanvasChatSurface(&window, draw);
    surface->show();
    if (stress)
    {
        window.resize(840, 240);
        surface->setGeometry(0, 0, 420, 240);
        auto *second = new chatterino::CanvasChatSurface(&window, draw);
        second->setGeometry(420, 0, 420, 240);
        second->show();
        auto *animation = new QTimer(&window);
        QObject::connect(animation, &QTimer::timeout, &window, [&, second] {
            row = QPixmap(1024, 1024);
            row.fill(QColor::fromHsv((frames++ * 11) % 360, 160, 160));
            surface->update();
            second->update();
            if (frames % 25 == 0) { second->hide(); second->show(); }
        });
        animation->start(20);
    }
    window.show();
    QTimer::singleShot(stress ? 8000 : 1500, &app, [&] {
        const auto image = surface->grabFramebuffer();
        if (image.isNull()) { qCritical() << "No GPU framebuffer"; app.exit(2); return; }
        const auto dpr = image.width() / 420.0;
        auto pixel = [&](int x, int y) { return image.pixelColor(qRound(x*dpr), qRound(y*dpr)); };
        const bool ok = pixel(5,5) == QColor("#151821") && pixel(40,80) == QColor("#20c080")
            && pixel(100,80) == QColor("#151821") && pixel(25,25) == QColor("#303648");
        if (argc > 1) image.save(QString::fromLocal8Bit(argv[1]));
        qInfo() << "Frames:" << frames;
        qInfo() << "Canvas GPU test:" << (stress ? frames >= 150 : ok) << image.size() << pixel(5,5) << pixel(40,80) << pixel(100,80) << pixel(25,25);
        app.exit(stress ? (frames >= 150 ? 0 : 4) : (ok ? 0 : 3));
    });
    return app.exec();
}
