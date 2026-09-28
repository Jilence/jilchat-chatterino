// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/NativeMessaging.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/Modes.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/IpcQueue.hpp"
#include "util/PostToThread.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStringBuilder>

#include <iostream>
#include <tuple>

#ifdef Q_OS_WIN
#    include "widgets/AttachedWindow.hpp"
#endif

namespace {

using namespace chatterino::nm::detail;
using namespace chatterino;
using namespace chatterino::literals;

const QString EXTENSION_ID = u"glknmaideaikkmemifbfkhnomoknepka"_s;
constexpr const size_t MESSAGE_SIZE = 1024;

struct Config {
#ifdef Q_OS_WIN
    QString fileName;
    QString registryKey;
#else
    QString browserDirectory;
    QString nmDirectory;
#endif
};

const Config FIREFOX{
#ifdef Q_OS_WIN
    .fileName = u"native-messaging-manifest-firefox.json"_s,
    .registryKey =
        u"HKCU\\Software\\Mozilla\\NativeMessagingHosts\\com.chatterino.chatterino"_s,
#elif defined(Q_OS_MACOS)
    .browserDirectory = u"~/Library/Application Support/Mozilla"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#else
    .browserDirectory = u"~/.mozilla"_s,
    .nmDirectory = u"native-messaging-hosts"_s,
#endif
};

const Config CHROME{
#ifdef Q_OS_WIN
    .fileName = u"native-messaging-manifest-chrome.json"_s,
    .registryKey =
        u"HKCU\\Software\\Google\\Chrome\\NativeMessagingHosts\\com.chatterino.chatterino"_s,
#elif defined(Q_OS_MACOS)
    .browserDirectory = u"~/Library/Application Support/Google/Chrome/"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#else
    .browserDirectory = u"~/.config/google-chrome"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#endif
};

ExpectedStr<void> registerNmManifest([[maybe_unused]] const Paths &paths,
                                     const Config &config,
                                     const QJsonDocument &document)
{
#ifdef Q_OS_WIN
    auto result =
        writeManifestTo(paths.miscDirectory, u"."_s, config.fileName, document);
    if (!result)
    {
        return makeUnexpected(result.error());
    }

    QSettings registry(config.registryKey, QSettings::NativeFormat);
    registry.setValue("Default",
                      QString(paths.miscDirectory % u'/' % config.fileName));
    registry.sync();
    if (registry.status() != QSettings::NoError)
    {
        return makeUnexpected(
            QString(u"Failed to register native messaging host in "_s %
                    config.registryKey));
    }
#else
    return writeManifestTo(config.browserDirectory, config.nmDirectory,
                           u"com.chatterino.chatterino.json"_s, document);
#endif
    return {};
}

}  // namespace

namespace chatterino::nm::detail {

ExpectedStr<void> writeManifestTo(QString directory, const QString &nmDirectory,
                                  const QString &filename,
                                  const QJsonDocument &json)
{
    if (directory.startsWith('~'))
    {
        directory = QDir::homePath() % QStringView{directory}.sliced(1);
    }

    QDir dir(directory);
    if (!dir.exists(nmDirectory) && !dir.mkdir(nmDirectory))
    {
        return makeUnexpected(
            QString(u"Failed to create " % nmDirectory % u" in " % directory));
    }
    dir.cd(nmDirectory);

    QFile file(dir.filePath(filename));
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
    {
        return makeUnexpected(
            QString(u"Failed to open " % filename % u" in " % directory));
    }
    const auto data = json.toJson();
    if (file.write(data) != data.size() || !file.flush())
    {
        return makeUnexpected(
            QString(u"Failed to write " % filename % u" in " % directory));
    }

    return {};
}

}  // namespace chatterino::nm::detail

namespace chatterino {

using namespace chatterino::nm::detail;
using namespace literals;

bool registerNmHost(const Paths &paths)
{
    auto getBaseDocument = [] {
        return QJsonObject{
            {u"name"_s, "com.chatterino.chatterino"_L1},
            {u"description"_s, "Browser interaction with chatterino."_L1},
            {u"path"_s, QCoreApplication::applicationFilePath()},
            {u"type"_s, "stdio"_L1},
        };
    };

    QStringList extensionIDs =
        getSettings()->additionalExtensionIDs.getValue().split(
            ';', Qt::SkipEmptyParts);

    const auto chromeRegistered = [&] {
        auto obj = getBaseDocument();
        QJsonArray allowedOriginsArr = {
            u"chrome-extension://%1/"_s.arg(EXTENSION_ID)};

        for (const auto &id : extensionIDs)
        {
            QString trimmedID = id.trimmed();
            if (!trimmedID.isEmpty())
            {
                allowedOriginsArr.append(
                    u"chrome-extension://%1/"_s.arg(trimmedID));
            }
        }

        obj.insert("allowed_origins", allowedOriginsArr);

        return registerNmManifest(paths, CHROME, QJsonDocument{obj});
    }();
    if (!chromeRegistered)
    {
        qCDebug(chatterinoNativeMessage)
            << "Chrome native messaging registration:"
            << chromeRegistered.error();
    }

    const auto firefoxRegistered = [&] {
        auto obj = getBaseDocument();
        QJsonArray allowedExtensions = {"chatterino_native@chatterino.com"};

        for (const auto &id : extensionIDs)
        {
            QString trimmedID = id.trimmed();
            if (!trimmedID.isEmpty())
            {
                allowedExtensions.append(trimmedID);
            }
        }

        obj.insert("allowed_extensions", allowedExtensions);

        return registerNmManifest(paths, FIREFOX, QJsonDocument{obj});
    }();
    if (!firefoxRegistered)
    {
        qCDebug(chatterinoNativeMessage)
            << "Firefox native messaging registration:"
            << firefoxRegistered.error();
    }

    return bool(chromeRegistered) && bool(firefoxRegistered);
}

void registerNmHost(Modes modes, const Paths &paths)
{
    if (modes.isPortable)
    {
        return;
    }

    std::ignore = registerNmHost(paths);
}

std::string &getNmQueueName(const Paths &paths)
{
    static std::string name =
        "chatterino_gui" + paths.applicationFilePathHash.toStdString();
    return name;
}

namespace nm::client {

void sendMessage(const QByteArray &array)
{
    ipc::sendMessage("chatterino_gui", array);
}

void writeToCout(const QByteArray &array)
{
    const auto *data = array.data();
    auto size = uint32_t(array.size());

    std::cout.write(reinterpret_cast<char *>(&size), 4);
    std::cout.write(data, size);
    std::cout.flush();
}

}  // namespace nm::client

NativeMessagingServer::NativeMessagingServer()
    : thread(new ReceiverThread(*this))
{
    this->thread->setObjectName("C2NMReceiver");
}

NativeMessagingServer::~NativeMessagingServer()
{
    if (this->detachTimer_)
    {
        this->detachTimer_->stop();
        this->detachTimer_.reset();
    }

    if (!ipc::IpcQueue::remove("chatterino_gui"))
    {
        qCWarning(chatterinoNativeMessage) << "Failed to remove message queue";
    }
    this->thread->requestInterruption();
    this->thread->quit();

    if (!this->thread->wait(100))
    {
        this->thread->terminate();

        if (!this->thread->wait(100))
        {
            qCWarning(chatterinoNativeMessage)
                << "Failed to terminate thread cleanly";
        }
    }
}

void NativeMessagingServer::noteActivity()
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    this->lastActivityMs_.store(now, std::memory_order_relaxed);
}

void NativeMessagingServer::start()
{
    this->noteActivity();
    this->thread->start();

    this->detachTimer_ = std::make_unique<QTimer>();
    this->detachTimer_->setInterval(5 * 1000);
    QObject::connect(this->detachTimer_.get(), &QTimer::timeout, [this] {
        if (!getSettings()->autoDetachLiveTab)
        {
            return;
        }

        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto last =
            this->lastActivityMs_.load(std::memory_order_relaxed);
        if (now - last < 10 * 1000)
        {
            return;
        }

        auto watching = getApp()->getTwitch()->getWatchingChannel().get();
        if (!watching || watching->isEmpty())
        {
            return;
        }

        // Avoid clearing again on every tick until the browser speaks.
        this->noteActivity();
        getApp()->getTwitch()->setWatchingChannel(Channel::getEmpty());
    });
    this->detachTimer_->start();
}

NativeMessagingServer::ReceiverThread::ReceiverThread(
    NativeMessagingServer &parent)
    : parent_(parent)
{
}

void NativeMessagingServer::ReceiverThread::run()
{
    auto [messageQueue, error] =
        ipc::IpcQueue::tryReplaceOrCreate("chatterino_gui", 100, MESSAGE_SIZE);

    if (!error.isEmpty())
    {
        qCDebug(chatterinoNativeMessage)
            << "Failed to create message queue:" << error;

        nmIpcError().set(error);
        return;
    }

    while (!this->isInterruptionRequested())
    {
        auto buf = messageQueue->receive();
        if (buf.isEmpty())
        {
            continue;
        }
        auto document = QJsonDocument::fromJson(buf);

        this->handleMessage(document.object());
    }
}

void NativeMessagingServer::ReceiverThread::handleMessage(
    const QJsonObject &root)
{
    QString action = root["action"_L1].toString();

    // select/sync/ping mean a Twitch tab is still open. detach does not:
    // that is the browser leaving the channel.
    if (action == "select" || action == "sync" || action == "ping")
    {
        this->parent_.noteActivity();
    }

    if (action == "select")
    {
        this->handleSelect(root);
        return;
    }
    if (action == "detach")
    {
        this->handleDetach(root);
        return;
    }
    if (action == "sync")
    {
        this->handleSync(root);
        return;
    }

    qCDebug(chatterinoNativeMessage) << "NM unknown action" << action;
}

void NativeMessagingServer::ReceiverThread::handleSelect(
    const QJsonObject &root)
{
    QString type = root["type"_L1].toString();
    bool attach = root["attach"_L1].toBool();
    bool attachFullscreen = root["attach_fullscreen"_L1].toBool();
    QString name = root["name"_L1].toString();

#ifdef USEWINSDK
    const auto sizeObject = root["size"_L1].toObject();
    AttachedWindow::GetArgs args = {
        .winId = root["winId"_L1].toString(),
        .yOffset = root["yOffset"_L1].toInt(-1),
        .x = sizeObject["x"_L1].toDouble(-1.0),
        .pixelRatio = sizeObject["pixelRatio"_L1].toDouble(-1.0),
        .width = sizeObject["width"_L1].toInt(-1),
        .height = sizeObject["height"_L1].toInt(-1),
        .fullscreen = attachFullscreen,
    };

    qCDebug(chatterinoNativeMessage)
        << args.x << args.pixelRatio << args.width << args.height << args.winId;

    if (args.winId.isNull())
    {
        qCDebug(chatterinoNativeMessage) << "winId in select is missing";
        return;
    }
#endif

    if (type != u"twitch"_s)
    {
        qCDebug(chatterinoNativeMessage) << "NM unknown channel type";
        return;
    }

    postToThread([=] {
        if (!name.isEmpty())
        {
            auto channel = getApp()->getTwitch()->getOrAddChannel(name);
            if (getApp()->getTwitch()->getWatchingChannel().get() != channel)
            {
                getApp()->getTwitch()->setWatchingChannel(channel);
            }
        }

        if (attach || attachFullscreen)
        {
#ifdef USEWINSDK
            auto *window = AttachedWindow::getForeground(args);
            if (!name.isEmpty())
            {
                window->setChannel(
                    getApp()->getTwitch()->getOrAddChannel(name));
            }
#endif
        }
    });
}

void NativeMessagingServer::ReceiverThread::handleDetach(
    const QJsonObject &root)
{
    QString winId = root["winId"_L1].toString();

    if (winId.isNull())
    {
        qCDebug(chatterinoNativeMessage) << "NM winId missing";
        return;
    }

    const bool autoDetach = getSettings()->autoDetachLiveTab;
    postToThread([winId, autoDetach] {
#ifdef USEWINSDK
        qCDebug(chatterinoNativeMessage) << "NW detach";
        AttachedWindow::detach(winId);
#endif
        if (!autoDetach)
        {
            return;
        }

        auto watching = getApp()->getTwitch()->getWatchingChannel().get();
        if (watching && !watching->isEmpty())
        {
            getApp()->getTwitch()->setWatchingChannel(Channel::getEmpty());
        }
    });
}

void NativeMessagingServer::ReceiverThread::handleSync(const QJsonObject &root)
{
    postToThread([&parent = this->parent_,
                  twitch = root["twitchChannels"_L1].toArray()] {
        parent.syncChannels(twitch);
    });
}

void NativeMessagingServer::syncChannels(const QJsonArray &twitchChannels)
{
    assertInGuiThread();

    std::vector<ChannelPtr> updated;
    updated.reserve(twitchChannels.size());
    for (const auto &value : twitchChannels)
    {
        auto name = value.toString();
        if (name.isEmpty())
        {
            continue;
        }

        updated.emplace_back(getApp()->getTwitch()->getOrAddChannel(name));
    }

    this->channelWarmer_ = std::move(updated);
}

Atomic<std::optional<QString>> &nmIpcError()
{
    static Atomic<std::optional<QString>> x;
    return x;
}

}  // namespace chatterino
