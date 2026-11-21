// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QJsonObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace chatterino {

class Paths;
class Settings;
class Modes;

class Updates
{
    const Paths &paths;
    const Modes &modes;

public:
    Updates(const Modes &modes_, const Paths &paths_, Settings &settings);

    enum Status {
        None,
        Searching,
        UpdateAvailable,
        NoUpdateAvailable,
        SearchFailed,
        Downloading,
        DownloadFailed,
        WriteFileFailed,
        MissingPortableUpdater,
        RunUpdaterFailed,
        /// The download didn't match the checksum its release lists
        VerifyFailed,
        /// Installing the Flatpak bundle didn't change the installed version
        FlatpakNotUpdated,
    };

    static bool isDowngradeOf(const QString &online, const QString &current);

    /// One line of what an update changes, for the update window
    struct Change {
        /// "new", "fixed", "removed" or "changed"
        QString kind;
        QString title;
        QString detail;
    };

    /// The changes in a release's notes. Lines like "- New Title. Detail."
    /// (from the changelog notes of the pull requests) are used when there
    /// are any; otherwise the commits listed there, by their prefix (feat,
    /// fix), leaving out ones users don't notice.
    static std::vector<Change> parseChanges(const QString &notes);

    void deleteOldFiles();

    void checkForUpdates();
    const QString &getCurrentVersion() const;
    const QString &getOnlineVersion() const;
    void installUpdates();
    Status getStatus() const;

    static QString portableUpdaterPath(const Paths &paths);

    bool shouldShowUpdateButton() const;
    /// Whether there's an update to install, also again after an install
    /// failed
    bool canInstall() const;
    bool isError() const;
    bool isDowngrade() const;

    /// Generates the string that the update dialog will show.
    QString buildUpdateAvailableText() const;

    /// What the update changes, from its release's notes
    const std::vector<Change> &getChanges() const;
    /// Whether installUpdates installs it here (otherwise it opens the
    /// download)
    bool installsItself() const;
    /// What went wrong, for the error statuses
    QString problemText() const;

    pajlada::Signals::Signal<Status> statusUpdated;

private:
    QString currentVersion_;
    QString onlineVersion_;
    std::vector<Change> changes_;
    Status status_ = None;
    bool isDowngrade_{};

    /// The installer (Windows) or disk image (macOS) of the release
    QString updateExe_;
    QString updateExeName_;
    /// Installs the downloaded Flatpak bundle `bundlePath` on the host, then
    /// restarts into it
    void installFlatpakBundle(const QString &bundlePath);

    /// The release's portable zip (Windows), for portable installs
    QString portableZip_;
    QString portableZipName_;
    /// The release's Flatpak bundle (Linux), for Flatpak installs
    QString flatpakBundle_;
    QString flatpakBundleName_;
    /// Set once a release with an update was found
    bool hasRelease_ = false;
    /// The release's page, for what isn't installed automatically
    QString updateGuideLink_;
    /// The release's sha256-checksums.txt
    QString checksumsUrl_;

    /// Checks again every few hours
    QTimer checkTimer_;

    /// Downloads `url`, checks it against the release's checksums and saves
    /// it as `fileName` in the misc directory; `done` gets its path.
    /// Remembers what `release` offers for this platform
    void takeRelease(const QJsonObject &release);
    void downloadVerified(const QString &url, const QString &assetName,
                          const QString &fileName,
                          std::function<void(const QString &)> done);

    void setStatus_(Status status);

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino
