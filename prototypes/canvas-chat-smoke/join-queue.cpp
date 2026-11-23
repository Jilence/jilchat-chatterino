#include "util/RatelimitBucket.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int joined = 0;
    chatterino::RatelimitBucket bucket(18, 100, [&](QString) { ++joined; }, &app);
    for (int i = 0; i < 40; ++i) bucket.send(QString::number(i));
    if (joined != 18) return 1;
    QTimer::singleShot(50, Qt::PreciseTimer, &app, [&] {
        if (joined != 18) app.exit(2);
    });
    QTimer::singleShot(150, Qt::PreciseTimer, &app, [&] {
        if (joined != 36) app.exit(3);
    });
    QTimer::singleShot(250, Qt::PreciseTimer, &app, [&] {
        qInfo() << "Join queue:" << joined << "/ 40, initial burst and cooldown respected";
        app.exit(joined == 40 ? 0 : 4);
    });
    return app.exec();
}
