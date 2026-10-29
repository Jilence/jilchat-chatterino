// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Literals.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QUrl>
#include <semver/semver.hpp>

#include <filesystem>
#include <system_error>

namespace {

using namespace chatterino;
using namespace literals;

const QString RELEASES_API =
    u"https://api.github.com/repos/Jilence/jilchat-chatterino"_s;
constexpr int FIRST_CHECK_DELAY_MS = 15 * 1000;
constexpr int CHECK_INTERVAL_MS = 3 * 60 * 60 * 1000;

#ifdef Q_OS_LINUX
/// The AppImage this runs from, empty when it isn't one
QString appImagePath()
{
    return qEnvironmentVariable("APPIMAGE");
}
#endif

#ifdef Q_OS_MACOS
/// Replaces the app bundle with the one in the disk image once this
/// instance has quit, then starts it again.
/// Arguments: the PID to wait for, the disk image, the bundle to replace.
constexpr const char *MACOS_UPDATE_SCRIPT =
    R"(#!/bin/bash
pid="$1"
dmg="$2"
app="$3"

while kill -0 "$pid" 2>/dev/null; do
    sleep 0.5
done

mnt="$(mktemp -d /tmp/jilchat-update.XXXXXX)" ||
    exit 1 if !hdiutil attach - nobrowse - readonly - noautoopen -
            mountpoint "$mnt"
                       "$dmg" >
        / dev / null;
then open "$app" exit 1 fi

    src = "$(find " $mnt " -maxdepth 1 -name '*.app' -print -quit)" new =
        "${app%.app}.updating.app" rm - rf "$new" if[-z "$src"] || !ditto
                                                                   "$src"
                                                                   "$new";
then hdiutil detach "$mnt" - quiet rm -
        rf "$new" open "$app" exit 1 fi hdiutil detach "$mnt" -
        quiet rmdir "$mnt" 2 >
    / dev /
        null

#    Swap the bundles; keep the old one until the new one is in place
            old = "${app%.app}.old.app" rm - rf "$old" if mv "$app"
                                                             "$old" &&
                  mv "$new"
                     "$app"; then
    rm -rf "$old"
else
    [ -d "$old" ] && [ ! -d "$app" ] && mv "$old" "$app"
    rm -rf "$new"
fi

xattr -dr com.apple.quarantine "$app" 2>/dev/null
rm -f "$dmg"
open "$app"
)";
#endif

}  // namespace

namespace chatterino {

Updates::Updates(const Modes &modes_, const Paths &paths_, Settings &settings)
    : paths(paths_)
    , modes(modes_)
    , currentVersion_(CHATTERINO_VERSION)
    , updateGuideLink_("https://chatterino.com")
{
    qCDebug(chatterinoUpdate) << "init UpdateManager";

    settings.betaUpdates.connect(
        [this] {
            this->checkForUpdates();
        },
        this->managedConnections, false);

    // what an update left behind the last time
    this->deleteOldFiles();

    // Once shortly after starting, then every few hours
    QTimer::singleShot(FIRST_CHECK_DELAY_MS, &this->checkTimer_, [this] {
        this->checkForUpdates();
    });
    this->checkTimer_.setInterval(CHECK_INTERVAL_MS);
    QObject::connect(&this->checkTimer_, &QTimer::timeout, &this->checkTimer_,
                     [this] {
                         this->checkForUpdates();
                     });
    this->checkTimer_.start();
}

bool Updates::isDowngradeOf(const QString &online, const QString &current)
{
    semver::version onlineVersion;
    if (!onlineVersion.from_string_noexcept(online.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse online version"
                                    << online << "into a proper semver string";
        return false;
    }

    semver::version currentVersion;
    if (!currentVersion.from_string_noexcept(current.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse current version"
                                    << current << "into a proper semver string";
        return false;
    }

    if (onlineVersion.major == 7)
    {
        onlineVersion.major = 2;
    }
    else if (currentVersion.major == 7 && onlineVersion.major == 2)
    {
        currentVersion = {2, currentVersion.minor, currentVersion.patch,
                          currentVersion.prerelease_type,
                          currentVersion.prerelease_number};
    }

    return onlineVersion < currentVersion;
}

void Updates::deleteOldFiles()
{
    std::ignore = QtConcurrent::run([dir{this->paths.miscDirectory}] {
        {
            auto path = combinePath(dir, "Update.exe");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
        {
            auto path = combinePath(dir, "update.zip");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
    });
}

const QString &Updates::getCurrentVersion() const
{
    return this->currentVersion_;
}

const QString &Updates::getOnlineVersion() const
{
    return this->onlineVersion_;
}

void Updates::installUpdates()
{
    if (this->status_ != UpdateAvailable)
    {
        assert(false);
        return;
    }

#ifdef Q_OS_WIN
    if (this->updateExe_.isEmpty())
    {
        // The release has no installer for this architecture
        QDesktopServices::openUrl(QUrl(this->updateGuideLink_));
        return;
    }

    this->setStatus_(Downloading);
    this->downloadVerified(
        this->updateExe_, this->updateExeName_, u"Update.exe"_s,
        [this](const QString &path) {
            // Without a window; the installer closes this instance and
            // starts the new version when it is done.
            if (!QProcess::startDetached(
                    path, {"/SILENT", "/SP-", "/SUPPRESSMSGBOXES",
                           "/CLOSEAPPLICATIONS"}))
            {
                this->setStatus_(RunUpdaterFailed);
                return;
            }
            QApplication::exit(0);
        });
#elif defined(Q_OS_MACOS)
    // The app bundle this runs from: <bundle>.app/Contents/MacOS/<binary>
    QDir bundle(QCoreApplication::applicationDirPath());
    bundle.cdUp();
    bundle.cdUp();
    const auto bundlePath = bundle.absolutePath();
    const QFileInfo bundleParent(QFileInfo(bundlePath).absolutePath());
    if (this->updateExe_.isEmpty() || !bundlePath.endsWith(u".app"_s) ||
        !bundleParent.isWritable())
    {
        // Not replaceable from here: open the disk image to install by hand
        QDesktopServices::openUrl(QUrl(this->updateExe_.isEmpty()
                                           ? this->updateGuideLink_
                                           : this->updateExe_));
        return;
    }

    this->setStatus_(Downloading);
    this->downloadVerified(
        this->updateExe_, this->updateExeName_, u"update.dmg"_s,
        [this, bundlePath](const QString &dmgPath) {
            const auto scriptPath =
                combinePath(this->paths.miscDirectory, u"update.sh"_s);
            QFile script(scriptPath);
            if (!script.open(QIODevice::Truncate | QIODevice::WriteOnly) ||
                script.write(MACOS_UPDATE_SCRIPT) == -1)
            {
                this->setStatus_(WriteFileFailed);
                return;
            }
            script.close();

            if (!QProcess::startDetached(
                    u"/bin/bash"_s,
                    {scriptPath,
                     QString::number(QCoreApplication::applicationPid()),
                     dmgPath, bundlePath}))
            {
                this->setStatus_(RunUpdaterFailed);
                return;
            }
            QApplication::exit(0);
        });
#elif defined(Q_OS_LINUX)
    // An AppImage is one file that can be swapped while it runs; other
    // installs (.deb) get the release page
    const auto appImage = appImagePath();
    if (this->updateExe_.isEmpty() || appImage.isEmpty() ||
        !QFileInfo(QFileInfo(appImage).absolutePath()).isWritable())
    {
        QDesktopServices::openUrl(QUrl(this->updateGuideLink_));
        return;
    }

    this->setStatus_(Downloading);
    this->downloadVerified(
        this->updateExe_, this->updateExeName_, u"update.AppImage"_s,
        [this, appImage](const QString &downloaded) {
            // Next to the old one first, so the swap is a rename on the same
            // file system
            const auto next = appImage + u".update"_s;
            QFile::remove(next);
            if (!QFile::copy(downloaded, next) ||
                !QFile::setPermissions(next,
                                       QFile::ReadOwner | QFile::WriteOwner |
                                           QFile::ExeOwner | QFile::ReadGroup |
                                           QFile::ExeGroup | QFile::ReadOther |
                                           QFile::ExeOther))
            {
                QFile::remove(next);
                this->setStatus_(WriteFileFailed);
                return;
            }
            QFile::remove(downloaded);

            std::error_code error;
            std::filesystem::rename(next.toStdString(), appImage.toStdString(),
                                    error);
            if (error)
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to replace the AppImage:"
                    << QString::fromStdString(error.message());
                QFile::remove(next);
                this->setStatus_(WriteFileFailed);
                return;
            }

            if (!QProcess::startDetached(appImage, {}))
            {
                this->setStatus_(RunUpdaterFailed);
                return;
            }
            QApplication::exit(0);
        });
#else
    QDesktopServices::openUrl(QUrl(this->updateGuideLink_));
#endif
}

void Updates::downloadVerified(const QString &url, const QString &assetName,
                               const QString &fileName,
                               std::function<void(const QString &)> done)
{
    if (this->checksumsUrl_.isEmpty())
    {
        qCWarning(chatterinoUpdate) << "The release has no checksums";
        this->setStatus_(VerifyFailed);
        return;
    }

    NetworkRequest(this->checksumsUrl_)
        .timeout(60000)
        .followRedirects(true)
        .onError([this](const NetworkResult &) {
            this->setStatus_(DownloadFailed);
        })
        .onSuccess([this, url, assetName, fileName,
                    done = std::move(done)](const NetworkResult &result) {
            // Lines of sha256sum: "<hash>  <name>" (or "<hash> *<name>")
            QString expected;
            const auto lines = QString::fromUtf8(result.getData()).split(u'\n');
            for (const auto &line : lines)
            {
                const auto parts = line.trimmed().split(
                    QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts);
                if (parts.size() == 2 &&
                    (parts[1] == assetName || parts[1] == u'*' + assetName))
                {
                    expected = parts[0].toLower();
                    break;
                }
            }
            if (expected.isEmpty())
            {
                qCWarning(chatterinoUpdate)
                    << "No checksum for" << assetName << "in the release";
                this->setStatus_(VerifyFailed);
                return;
            }

            NetworkRequest(url)
                .timeout(600000)
                .followRedirects(true)
                .onError([this](const NetworkResult &) {
                    this->setStatus_(DownloadFailed);
                })
                .onSuccess([this, expected, fileName,
                            done](const NetworkResult &download) {
                    const auto &data = download.getData();
                    const auto actual =
                        QString::fromLatin1(
                            QCryptographicHash::hash(data,
                                                     QCryptographicHash::Sha256)
                                .toHex())
                            .toLower();
                    if (actual != expected)
                    {
                        qCWarning(chatterinoUpdate)
                            << "Update checksum mismatch:" << actual
                            << "expected" << expected;
                        this->setStatus_(VerifyFailed);
                        return;
                    }

                    const auto path =
                        combinePath(this->paths.miscDirectory, fileName);
                    QFile file(path);
                    if (!file.open(QIODevice::Truncate |
                                   QIODevice::WriteOnly) ||
                        file.write(data) != data.size())
                    {
                        qCWarning(chatterinoUpdate)
                            << "Failed to save" << path << file.errorString();
                        this->setStatus_(WriteFileFailed);
                        return;
                    }
                    file.close();
                    done(path);
                })
                .execute();
        })
        .execute();
}

void Updates::checkForUpdates()
{
#ifndef CHATTERINO_DISABLE_UPDATER
    const auto &version = Version::instance();

    if (!version.isSupportedOS() || version.isFlatpak())
    {
        return;
    }

#    ifdef Q_OS_WIN
    // Only what the installer put there updates itself: it leaves its
    // uninstaller next to the program. Portable packages and plain zips are
    // updated by hand.
    if (this->modes.isPortable ||
        !QFile::exists(combinePath(QCoreApplication::applicationDirPath(),
                                   u"unins000.exe"_s)))
    {
        qCDebug(chatterinoUpdate)
            << "Update checking skipped: not installed by the installer";
        return;
    }
#    endif

    // Releases are matched by commit, so a build has to know its own. Builds
    // made elsewhere than this repository aren't in it and get no updates.
    const auto &commit = version.commitHash();
    if (commit.isEmpty() || commit.startsWith(u"GIT-"_s))
    {
        qCDebug(chatterinoUpdate)
            << "Update checking skipped: the build doesn't know its commit";
        return;
    }

    if (this->status_ == Downloading || this->status_ == Searching)
    {
        return;
    }
    this->setStatus_(Searching);

    NetworkRequest(QUrl(RELEASES_API + u"/releases?per_page=30"_s))
        .header("Accept", "application/vnd.github+json")
        .timeout(60000)
        .onError([this](const NetworkResult &result) {
            qCDebug(chatterinoUpdate)
                << "Failed to load releases:" << result.formatError();
            this->setStatus_(None);
        })
        .onSuccess([this, commit](const NetworkResult &result) {
            // The newest release, Latest only or also pre-releases with
            // "Receive beta updates"
            const bool beta = getSettings()->betaUpdates;
            QJsonObject release;
            const auto releases = result.parseJsonArray();
            for (const auto &value : releases)
            {
                const auto o = value.toObject();
                if (o["draft"_L1].toBool() ||
                    (o["prerelease"_L1].toBool() && !beta))
                {
                    continue;
                }
                release = o;
                break;
            }
            const auto tag = release["tag_name"_L1].toString();
            if (tag.isEmpty())
            {
                this->setStatus_(NoUpdateAvailable);
                return;
            }

            // Newer than this build when it comes after this build's commit
            NetworkRequest(
                QUrl(RELEASES_API + u"/compare/"_s + commit + u"..."_s + tag))
                .header("Accept", "application/vnd.github+json")
                .timeout(60000)
                .onError([this](const NetworkResult &) {
                    // e.g. this build's commit isn't in the repository
                    this->setStatus_(NoUpdateAvailable);
                })
                .onSuccess([this, release](const NetworkResult &compare) {
                    if (compare.parseJson().value("status"_L1).toString() !=
                        u"ahead"_s)
                    {
                        this->setStatus_(NoUpdateAvailable);
                        return;
                    }
                    this->takeRelease(release);
                    this->setStatus_(UpdateAvailable);
                })
                .execute();
        })
        .execute();
#endif
}

void Updates::takeRelease(const QJsonObject &release)
{
    // "Title, tag", or only one of them when the other is missing or the same
    const auto name = release["name"_L1].toString().trimmed();
    const auto tag = release["tag_name"_L1].toString().trimmed();
    if (name.isEmpty() || name == tag)
    {
        this->onlineVersion_ = tag;
    }
    else if (tag.isEmpty())
    {
        this->onlineVersion_ = name;
    }
    else
    {
        this->onlineVersion_ = name + u", "_s + tag;
    }
    this->updateGuideLink_ = release["html_url"_L1].toString();
    this->updateExe_.clear();
    this->updateExeName_.clear();
    this->checksumsUrl_.clear();

    const auto assets = release.value("assets"_L1).toArray();
    for (const auto &value : assets)
    {
        const auto asset = value.toObject();
        const auto assetName = asset["name"_L1].toString();
        const auto url = asset["browser_download_url"_L1].toString();
        if (assetName == u"sha256-checksums.txt"_s)
        {
            this->checksumsUrl_ = url;
            continue;
        }

#ifdef Q_OS_WIN
        // ARM installers are marked as such, everything else is x64
        const bool arm =
            assetName.startsWith(u"Experimental-ARM64-"_s, Qt::CaseInsensitive);
#    ifdef Q_PROCESSOR_ARM
        const bool otherArchitecture = !arm;
#    else
        const bool otherArchitecture = arm;
#    endif
        if (!otherArchitecture &&
            assetName.endsWith(u".Installer.exe"_s, Qt::CaseInsensitive))
        {
            this->updateExe_ = url;
            this->updateExeName_ = assetName;
        }
#elif defined(Q_OS_MACOS)
        // one universal disk image for Intel and Apple silicon
        if (assetName.endsWith(u".dmg"_s, Qt::CaseInsensitive))
        {
            this->updateExe_ = url;
            this->updateExeName_ = assetName;
        }
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
        // AppImages are only built for x86_64
        if (assetName.endsWith(u"x86_64.AppImage"_s, Qt::CaseInsensitive))
        {
            this->updateExe_ = url;
            this->updateExeName_ = assetName;
        }
#endif
    }
}

Updates::Status Updates::getStatus() const
{
    return this->status_;
}

QString Updates::portableUpdaterPath(const Paths &paths)
{
    return combinePath(paths.rootAppDataDirectory,
                       "updater.1/ChatterinoUpdater.exe");
}

bool Updates::shouldShowUpdateButton() const
{
    switch (this->getStatus())
    {
        case UpdateAvailable:
        case SearchFailed:
        case Downloading:
        case DownloadFailed:
        case WriteFileFailed:
        case VerifyFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isError() const
{
    switch (this->getStatus())
    {
        case SearchFailed:
        case DownloadFailed:
        case WriteFileFailed:
        case MissingPortableUpdater:
        case RunUpdaterFailed:
        case VerifyFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isDowngrade() const
{
    return this->isDowngrade_;
}

QString Updates::buildUpdateAvailableText() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const bool installs = true;
#elif defined(Q_OS_LINUX)
    const bool installs = !appImagePath().isEmpty();
#else
    const bool installs = false;
#endif
    if (installs)
    {
        return QString("An update (%1) is available.\n\nDo you want to "
                       "download and install it? JilChat Chatterino restarts "
                       "afterwards.")
            .arg(this->getOnlineVersion());
    }
    return QString("An update (%1) is available.\n\nDo you want to open "
                   "the download?")
        .arg(this->getOnlineVersion());
}

void Updates::setStatus_(Status status)
{
    if (this->status_ != status)
    {
        this->status_ = status;
        postToThread([this, status] {
            this->statusUpdated.invoke(status);
        });
    }
}

}  // namespace chatterino
