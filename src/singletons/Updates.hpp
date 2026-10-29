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
    };

    static bool isDowngradeOf(const QString &online, const QString &current);

    void deleteOldFiles();

    void checkForUpdates();
    const QString &getCurrentVersion() const;
    const QString &getOnlineVersion() const;
    void installUpdates();
    Status getStatus() const;

    static QString portableUpdaterPath(const Paths &paths);

    bool shouldShowUpdateButton() const;
    bool isError() const;
    bool isDowngrade() const;

    /// Generates the string that the update dialog will show.
    QString buildUpdateAvailableText() const;

    pajlada::Signals::Signal<Status> statusUpdated;

private:
    QString currentVersion_;
    QString onlineVersion_;
    Status status_ = None;
    bool isDowngrade_{};

    /// The installer (Windows) or disk image (macOS) of the release
    QString updateExe_;
    QString updateExeName_;
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
