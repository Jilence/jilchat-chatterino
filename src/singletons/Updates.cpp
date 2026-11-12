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
#include <QSettings>
#include <QtConcurrent>
#include <QUrl>
#include <semver/semver.hpp>

#include <filesystem>
#include <optional>
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

/// The running Flatpak, as described by /.flatpak-info
struct FlatpakInstance {
    QString appId;
    QString branch;
    QString commit;
    QString instanceId;
    /// Whether it's installed for all users rather than only this one
    bool system = false;
};

std::optional<FlatpakInstance> readFlatpakInstance()
{
    const QSettings info(u"/.flatpak-info"_s, QSettings::IniFormat);
    FlatpakInstance instance{
        .appId = info.value(u"Application/name"_s).toString(),
        .branch = info.value(u"Instance/branch"_s).toString(),
        .commit = info.value(u"Instance/app-commit"_s).toString(),
        .instanceId = info.value(u"Instance/instance-id"_s).toString(),
        .system = info.value(u"Instance/app-path"_s)
                      .toString()
                      .startsWith(u"/var/lib/flatpak/"_s),
    };
    if (instance.appId.isEmpty() || instance.branch.isEmpty() ||
        instance.commit.isEmpty() || instance.instanceId.isEmpty())
    {
        return std::nullopt;
    }
    return instance;
}

// Runs on the host with the app id, branch, running commit, bundle and
// installation (--user or --system). Exits with 3 if the installed version is
// still the running one, so we don't restart into the same version.
const QString FLATPAK_INSTALL_SCRIPT = uR"sh(
flatpak install "$5" -y --noninteractive --reinstall --bundle "$4" || exit
[ "$(flatpak info "$5" --show-commit "$1//$2")" != "$3" ] || exit 3
)sh"_s;

// Runs on the host with the app id, branch and instance id. Starts the new
// version once this instance is gone. It runs in the background so
// flatpak-spawn returns right away, which would otherwise keep this instance
// alive.
const QString FLATPAK_RESTART_SCRIPT = uR"sh(
(
    i=0
    while [ "$i" -lt 30 ] && flatpak ps --columns=instance | grep -qxF "$3"; do
        sleep 1
        i=$((i + 1))
    done
    exec flatpak run --branch="$2" "$1"
) </dev/null >/dev/null 2>&1 &
)sh"_s;
#endif

#ifdef Q_OS_WIN
/// Starts the portable updater on `zipPath`. It waits for this instance to
/// exit, but can only restart "chatterino.exe", so cmd runs it and starts this
/// program again once it succeeded.
bool startPortableUpdater(const QString &updaterPath, const QString &zipPath)
{
    const auto command = uR"(/d /s /c ""%1" "%2" && start "" "%3"")"_s.arg(
        QDir::toNativeSeparators(updaterPath),
        QDir::toNativeSeparators(zipPath),
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));

    auto cmd = qEnvironmentVariable("ComSpec");
    if (cmd.isEmpty())
    {
        cmd = u"cmd.exe"_s;
    }

    QProcess process;
    process.setProgram(cmd);
    process.setNativeArguments(command);
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= 0x08000000;  // CREATE_NO_WINDOW
        });
    return process.startDetached();
}
#endif

#ifdef Q_OS_MACOS
/// Replaces the app bundle with the one in the disk image once this
/// instance has quit, then starts it again.
/// Arguments: the PID to wait for, the disk image, the bundle to replace.
// clang-format off
constexpr const char *MACOS_UPDATE_SCRIPT =
    R"SH(#!/bin/bash
pid="$1"
dmg="$2"
app="$3"

while kill -0 "$pid" 2>/dev/null; do
    sleep 0.5
done

mnt="$(mktemp -d /tmp/jilchat-update.XXXXXX)" || exit 1
if ! hdiutil attach -nobrowse -readonly -noautoopen \
        -mountpoint "$mnt" "$dmg" >/dev/null; then
    open "$app"
    exit 1
fi

src="$(find "$mnt" -maxdepth 1 -name '*.app' -print -quit)"
new="${app%.app}.updating.app"
rm -rf "$new"
if [ -z "$src" ] || ! ditto "$src" "$new"; then
    hdiutil detach "$mnt" -quiet
    rm -rf "$new"
    open "$app"
    exit 1
fi
hdiutil detach "$mnt" -quiet
rmdir "$mnt" 2>/dev/null

# Swap the bundles; keep the old one until the new one is in place
old="${app%.app}.old.app"
rm -rf "$old"
if mv "$app" "$old" && mv "$new" "$app"; then
    rm -rf "$old"
else
    [ -d "$old" ] && [ ! -d "$app" ] && mv "$old" "$app"
    rm -rf "$new"
fi

xattr -dr com.apple.quarantine "$app" 2>/dev/null
rm -f "$dmg"
open "$app"
)SH";
// clang-format on
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
    if (!this->canInstall())
    {
        assert(false);
        return;
    }

#ifdef Q_OS_WIN
    if (this->modes.isPortable)
    {
        if (this->portableZip_.isEmpty())
        {
            // The release has no zip for this architecture
            QDesktopServices::openUrl(QUrl(this->updateGuideLink_));
            return;
        }
        const auto updater = Updates::portableUpdaterPath(this->paths);
        if (!QFile::exists(updater))
        {
            this->setStatus_(MissingPortableUpdater);
            return;
        }

        this->setStatus_(Downloading);
        this->downloadVerified(this->portableZip_, this->portableZipName_,
                               u"update.zip"_s,
                               [this, updater](const QString &path) {
                                   if (!startPortableUpdater(updater, path))
                                   {
                                       this->setStatus_(RunUpdaterFailed);
                                       return;
                                   }
                                   QApplication::exit(0);
                               });
        return;
    }

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
            // The setup waits for this instance to close, installs into the
            // folder this runs from and starts the new version when it's done
            if (!QProcess::startDetached(
                    path, {u"--update"_s, u"--dest"_s,
                           QDir::toNativeSeparators(
                               QCoreApplication::applicationDirPath())}))
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
    if (Version::instance().isFlatpak())
    {
        if (this->flatpakBundle_.isEmpty())
        {
            QDesktopServices::openUrl(QUrl(this->updateGuideLink_));
            return;
        }
        this->setStatus_(Downloading);
        this->downloadVerified(this->flatpakBundle_, this->flatpakBundleName_,
                               u"update.flatpak"_s,
                               [this](const QString &path) {
                                   this->installFlatpakBundle(path);
                               });
        return;
    }

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

void Updates::installFlatpakBundle(const QString &bundlePath)
{
#ifdef Q_OS_LINUX
    const auto instance = readFlatpakInstance();
    if (!instance)
    {
        qCWarning(chatterinoUpdate) << "Failed to read /.flatpak-info";
        this->setStatus_(RunUpdaterFailed);
        return;
    }

    qCDebug(chatterinoUpdate) << "Installing the Flatpak bundle" << bundlePath;

    auto *process = new QProcess;
    process->setProcessChannelMode(QProcess::MergedChannels);

    QObject::connect(process, &QProcess::errorOccurred, process,
                     [this, process](QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart)
                         {
                             return;
                         }
                         qCWarning(chatterinoUpdate)
                             << "Failed to start flatpak-spawn:"
                             << process->errorString();
                         this->setStatus_(RunUpdaterFailed);
                         process->deleteLater();
                     });

    QObject::connect(
        process, &QProcess::finished, process,
        [this, process, bundlePath, instance = *instance](
            int exitCode, QProcess::ExitStatus exitStatus) {
            process->deleteLater();
            QFile::remove(bundlePath);

            if (exitStatus != QProcess::NormalExit || exitCode != 0)
            {
                qCWarning(chatterinoUpdate)
                    << "Installing the Flatpak bundle failed with exit code"
                    << exitCode << process->readAll();
                this->setStatus_(exitStatus == QProcess::NormalExit &&
                                         exitCode == 3
                                     ? FlatpakNotUpdated
                                     : RunUpdaterFailed);
                return;
            }

            // only quit once the host has the restart, it's quick
            QProcess restart;
            restart.start(
                u"flatpak-spawn"_s,
                {u"--host"_s, u"sh"_s, u"-c"_s, FLATPAK_RESTART_SCRIPT, u"sh"_s,
                 instance.appId, instance.branch, instance.instanceId});
            if (!restart.waitForFinished(10000) || restart.exitCode() != 0)
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to restart after the update"
                    << restart.errorString();
            }

            QApplication::exit(0);
        });

    // The bundle is in the app's data directory, which has the same path on
    // the host
    process->start(
        u"flatpak-spawn"_s,
        {u"--host"_s, u"sh"_s, u"-c"_s, FLATPAK_INSTALL_SCRIPT, u"sh"_s,
         instance->appId, instance->branch, instance->commit, bundlePath,
         instance->system ? u"--system"_s : u"--user"_s});
#else
    (void)bundlePath;
    this->setStatus_(RunUpdaterFailed);
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

    if (!version.isSupportedOS())
    {
        return;
    }

#    ifdef Q_OS_WIN
    // What the setup put there (it leaves its uninstaller next to the
    // program, earlier versions uninstall.ps1, the Inno Setup installer of
    // older versions its unins000.exe) and
    // portable packages update themselves. Plain zips are updated by hand.
    const auto appDir = QCoreApplication::applicationDirPath();
    if (!this->modes.isPortable &&
        !QFile::exists(
            combinePath(appDir, u"Uninstall JilChat Chatterino.exe"_s)) &&
        !QFile::exists(combinePath(appDir, u"uninstall.ps1"_s)) &&
        !QFile::exists(combinePath(appDir, u"unins000.exe"_s)))
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
    this->changes_ = Updates::parseChanges(release["body"_L1].toString());
    this->updateExe_.clear();
    this->updateExeName_.clear();
    this->portableZip_.clear();
    this->portableZipName_.clear();
    this->flatpakBundle_.clear();
    this->flatpakBundleName_.clear();
    this->checksumsUrl_.clear();
    this->hasRelease_ = true;

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

        // The portable package, with the "modes" file and the portable
        // updater: JilChatChatterino.Nightly.Portable.zip, with the
        // "Experimental-ARM64-" prefix for ARM like the installer. The plain
        // chatterino-windows-*.zip have neither, so they aren't used.
        if (!otherArchitecture &&
            assetName.endsWith(u".Portable.zip"_s, Qt::CaseInsensitive))
        {
            this->portableZip_ = url;
            this->portableZipName_ = assetName;
        }
#elif defined(Q_OS_MACOS)
        // one universal disk image for Intel and Apple silicon
        if (assetName.endsWith(u".dmg"_s, Qt::CaseInsensitive))
        {
            this->updateExe_ = url;
            this->updateExeName_ = assetName;
        }
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
        // AppImages and the Flatpak bundle are only built for x86_64
        if (assetName.endsWith(u"x86_64.AppImage"_s, Qt::CaseInsensitive))
        {
            this->updateExe_ = url;
            this->updateExeName_ = assetName;
        }
        if (assetName.endsWith(u".flatpak"_s, Qt::CaseInsensitive))
        {
            this->flatpakBundle_ = url;
            this->flatpakBundleName_ = assetName;
        }
#endif
    }
}

std::vector<Updates::Change> Updates::parseChanges(const QString &notes)
{
    static const QRegularExpression bullet(u"^\\s*[-*]\\s+(.+)$"_s);
    static const QRegularExpression written(
        u"^(New|Added|Add|Fixed|Fix|Removed|Remove|Changed|Change)\\b\\s*:?\\s*(.+)$"_s,
        QRegularExpression::CaseInsensitiveOption);
    // "- [2026-11-10] feat(raids): ..." as the release workflow writes them
    static const QRegularExpression commit(
        u"^(?:\\[[^\\]]*\\]\\s*)?([a-z]+)(?:\\([^)]*\\))?(!)?:\\s*(.+)$"_s);
    static const QRegularExpression sentenceEnd(u"(?<=[.!?])\\s+"_s);
    constexpr size_t maxChanges = 12;

    const auto capitalized = [](QString text) {
        text = text.trimmed();
        if (!text.isEmpty())
        {
            text[0] = text[0].toUpper();
        }
        return text;
    };

    std::vector<Change> fromNotes;
    std::vector<Change> fromCommits;
    const auto lines = notes.split(u'\n');
    for (const auto &line : lines)
    {
        const auto item = bullet.match(line.trimmed());
        if (!item.hasMatch())
        {
            continue;
        }
        const auto text = item.captured(1).trimmed();

        if (const auto match = written.match(text); match.hasMatch())
        {
            const auto word = match.captured(1).toLower();
            QString kind = u"changed"_s;
            if (word.startsWith(u"add"_s) || word == u"new"_s)
            {
                kind = u"new"_s;
            }
            else if (word.startsWith(u"fix"_s))
            {
                kind = u"fixed"_s;
            }
            else if (word.startsWith(u"remove"_s))
            {
                kind = u"removed"_s;
            }
            const auto sentences =
                match.captured(2).split(sentenceEnd, Qt::SkipEmptyParts);
            auto title = capitalized(sentences.value(0));
            if (title.endsWith(u'.') || title.endsWith(u'!'))
            {
                title.chop(1);
            }
            fromNotes.push_back({
                .kind = kind,
                .title = title,
                .detail = sentences.mid(1).join(u' ').trimmed(),
            });
            continue;
        }

        if (const auto match = commit.match(text); match.hasMatch())
        {
            const auto type = match.captured(1);
            QString kind;
            if (type == u"feat"_s)
            {
                kind = u"new"_s;
            }
            else if (type == u"fix"_s)
            {
                kind = u"fixed"_s;
            }
            else if (type == u"perf"_s || type == u"revert"_s)
            {
                kind = u"changed"_s;
            }
            else
            {
                // ci, chore, docs, refactor, test, ...: nothing users notice
                continue;
            }
            fromCommits.push_back({
                .kind = kind,
                .title = capitalized(match.captured(3)),
                .detail = {},
            });
        }
    }

    auto &changes = fromNotes.empty() ? fromCommits : fromNotes;
    if (changes.size() > maxChanges)
    {
        changes.resize(maxChanges);
    }
    return changes;
}

const std::vector<Updates::Change> &Updates::getChanges() const
{
    return this->changes_;
}

QString Updates::problemText() const
{
    switch (this->status_)
    {
        case SearchFailed:
            return u"Failed to load version information."_s;
        case DownloadFailed:
            return u"Failed to download the update. Check your connection "
                   "and try again."_s;
        case WriteFileFailed:
            return u"Failed to write the update to disk."_s;
        case MissingPortableUpdater:
            return u"The portable updater (expected in "_s +
                   Updates::portableUpdaterPath(this->paths) +
                   u") was not found."_s;
        case RunUpdaterFailed:
            return u"Failed to run the updater."_s;
        case VerifyFailed:
            return u"The update could not be verified against the checksums "
                   "of its release, so it was not installed."_s;
        case FlatpakNotUpdated:
            return u"Flatpak installed the update, but the same version is "
                   "still installed. Try again in a few minutes."_s;
        default:
            return {};
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
        case MissingPortableUpdater:
        case RunUpdaterFailed:
        case FlatpakNotUpdated:
            return true;

        default:
            return false;
    }
}

bool Updates::canInstall() const
{
    return this->hasRelease_ && this->status_ != Downloading &&
           this->status_ != Searching;
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
        case FlatpakNotUpdated:
            return true;

        default:
            return false;
    }
}

bool Updates::isDowngrade() const
{
    return this->isDowngrade_;
}

bool Updates::installsItself() const
{
#if defined(Q_OS_WIN)
    return !this->modes.isPortable ||
           (!this->portableZip_.isEmpty() &&
            QFile::exists(Updates::portableUpdaterPath(this->paths)));
#elif defined(Q_OS_MACOS)
    return true;
#elif defined(Q_OS_LINUX)
    return !appImagePath().isEmpty() ||
           (Version::instance().isFlatpak() && !this->flatpakBundle_.isEmpty());
#else
    return false;
#endif
}

QString Updates::buildUpdateAvailableText() const
{
#if defined(Q_OS_WIN)
    const bool installs =
        !this->modes.isPortable ||
        (!this->portableZip_.isEmpty() &&
         QFile::exists(Updates::portableUpdaterPath(this->paths)));
#elif defined(Q_OS_MACOS)
    const bool installs = true;
#elif defined(Q_OS_LINUX)
    const bool installs =
        !appImagePath().isEmpty() ||
        (Version::instance().isFlatpak() && !this->flatpakBundle_.isEmpty());
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
