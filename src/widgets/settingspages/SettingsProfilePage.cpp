// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/SettingsProfilePage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/chat/ChatAutomationController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CombinePath.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QWidget>

#include <tuple>

namespace {

using namespace chatterino;
using namespace chatterino::literals;

const QString SETTINGS_EXPORT_FORMAT = u"leafyrino-settings-export"_s;
const QString SETTINGS_PENDING_IMPORT_FILENAME =
    u"pending-settings-import.json"_s;
const QStringList SETTINGS_EXPORT_FILES = {
    u"settings.json"_s,
    u"commands.json"_s,
    WindowManager::WINDOW_LAYOUT_FILENAME,
    u"chat-automations.json"_s,
    u"user-data.json"_s,
};
/// A background bigger than this stays out of the export
constexpr qint64 MAX_EXPORTED_WALLPAPER_SIZE = 100LL * 1024 * 1024;

/// The background a theme file uses, if it is one of the managed ones in
/// the Wallpapers folder next to the themes
QString managedWallpaperOf(const QJsonObject &theme,
                           const QString &wallpapersDirectory)
{
    const auto source = theme[u"metadata"_s]
                            .toObject()[u"moltorino"_s]
                            .toObject()[u"wallpaper"_s]
                            .toObject()[u"source"_s]
                            .toString();
    if (source.isEmpty())
    {
        return {};
    }
    const QFileInfo info(source);
    if (QDir::cleanPath(info.absolutePath()) !=
            QDir::cleanPath(wallpapersDirectory) ||
        !info.isFile())
    {
        return {};
    }
    return info.fileName();
}

QJsonDocument readJsonFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = file.errorString();
        }
        return {};
    }

    QJsonParseError parseError;
    auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        if (error)
        {
            *error = parseError.errorString();
        }
        return {};
    }

    if (!document.isObject())
    {
        if (error)
        {
            *error = u"Expected a JSON object."_s;
        }
        return {};
    }

    return document;
}

bool writeJsonFile(const QString &path, const QJsonDocument &document,
                   QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        if (error)
        {
            *error = file.errorString();
        }
        return false;
    }

    file.write(document.toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        if (error)
        {
            *error = file.errorString();
        }
        return false;
    }

    return true;
}

QJsonObject sanitizeSettingsObject(QJsonObject settings)
{
    settings.remove(u"accounts"_s);
    settings.remove(u"kickAccounts"_s);
    return settings;
}

void exportSettingsProfile(QWidget *parent)
{
    if (auto *automations = getApp()->getChatAutomations())
    {
        std::ignore = automations->save();
    }
    getApp()->getCommands()->save();
    getApp()->getHotkeys()->save();
    getApp()->getWindows()->save();
    getSettings()->requestSave();

    const auto fileName = QFileDialog::getSaveFileName(
        parent, "Export JilChat Chatterino settings",
        QDir::homePath() + "/jilchat-chatterino-settings.json",
        "JilChat Chatterino settings (*.json);;JSON files (*.json)");
    if (fileName.isEmpty())
    {
        return;
    }

    const auto includeWallpapers = QMessageBox::question(
        parent, "Export settings",
        "Include the background images and videos of your own themes?\n\n"
        "They can make the export file large.",
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (includeWallpapers == QMessageBox::Cancel)
    {
        return;
    }

    QJsonObject files;
    for (const auto &relativeFile : SETTINGS_EXPORT_FILES)
    {
        const auto path =
            combinePath(getApp()->getPaths().settingsDirectory, relativeFile);
        if (!QFileInfo::exists(path))
        {
            continue;
        }

        QString error;
        auto document = readJsonFile(path, &error);
        if (document.isNull())
        {
            QMessageBox::critical(
                parent, "Export failed",
                "Could not read " + relativeFile + ": " + error);
            return;
        }

        auto object = document.object();
        if (relativeFile == u"settings.json"_s)
        {
            object = sanitizeSettingsObject(object);
        }
        files.insert(relativeFile, object);
    }

    // own themes, and the backgrounds they use
    const QDir themesDirectory(getApp()->getPaths().themesDirectory);
    const auto wallpapersDirectory = themesDirectory.filePath(u"Wallpapers"_s);
    QJsonObject themes;
    QJsonObject wallpapers;
    QStringList skippedWallpapers;
    const auto themeFiles =
        themesDirectory.entryInfoList({u"*.json"_s}, QDir::Files);
    for (const auto &entry : themeFiles)
    {
        auto theme = readJsonFile(entry.absoluteFilePath(), nullptr);
        if (theme.isNull())
        {
            continue;
        }
        themes.insert(entry.fileName(), theme.object());

        if (includeWallpapers != QMessageBox::Yes)
        {
            continue;
        }
        const auto wallpaper =
            managedWallpaperOf(theme.object(), wallpapersDirectory);
        if (wallpaper.isEmpty() || wallpapers.contains(wallpaper))
        {
            continue;
        }
        QFile file(QDir(wallpapersDirectory).filePath(wallpaper));
        if (file.size() > MAX_EXPORTED_WALLPAPER_SIZE ||
            !file.open(QIODevice::ReadOnly))
        {
            skippedWallpapers.append(wallpaper);
            continue;
        }
        wallpapers.insert(wallpaper,
                          QString::fromLatin1(file.readAll().toBase64()));
    }

    QJsonObject exportObject;
    exportObject.insert(u"format"_s, SETTINGS_EXPORT_FORMAT);
    exportObject.insert(u"version"_s, 2);
    exportObject.insert(u"files"_s, files);
    exportObject.insert(u"themes"_s, themes);
    if (!wallpapers.isEmpty())
    {
        exportObject.insert(u"wallpapers"_s, wallpapers);
    }

    QString error;
    if (!writeJsonFile(fileName, QJsonDocument(exportObject), &error))
    {
        QMessageBox::critical(parent, "Export failed",
                              "Could not write export file: " + error);
        return;
    }

    auto message =
        QStringLiteral("Settings were exported without Twitch/Kick accounts.");
    if (!skippedWallpapers.isEmpty())
    {
        message += u"\n\nThese backgrounds were too large and were left "
                   u"out: "_s +
                   skippedWallpapers.join(u", "_s);
    }
    QMessageBox::information(parent, "Export complete", message);
}

void importSettingsProfile(QWidget *parent)
{
    const auto fileName = QFileDialog::getOpenFileName(
        parent, "Import JilChat Chatterino settings", QDir::homePath(),
        "JilChat Chatterino settings (*.json);;JSON files (*.json)");
    if (fileName.isEmpty())
    {
        return;
    }

    QString error;
    auto exportDocument = readJsonFile(fileName, &error);
    if (exportDocument.isNull())
    {
        QMessageBox::critical(parent, "Import failed",
                              "Could not read import file: " + error);
        return;
    }

    const auto exportObject = exportDocument.object();
    if (exportObject[u"format"_s].toString() != SETTINGS_EXPORT_FORMAT ||
        !exportObject[u"files"_s].isObject())
    {
        QMessageBox::critical(
            parent, "Import failed",
            "This is not a JilChat Chatterino settings export.");
        return;
    }

    auto reply = QMessageBox::question(
        parent, "Import settings",
        "Importing will replace local settings, hotkeys, highlights, "
        "moderation buttons, commands, tab layout, chat automations and user "
        "notes, and add the exported themes. Twitch/Kick accounts on this "
        "device will be kept.\n\nContinue?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
    {
        return;
    }

    const auto pendingPath = combinePath(getApp()->getPaths().settingsDirectory,
                                         SETTINGS_PENDING_IMPORT_FILENAME);
    if (!writeJsonFile(pendingPath, exportDocument, &error))
    {
        QMessageBox::critical(parent, "Import failed",
                              "Could not stage import file: " + error);
        return;
    }

    QMessageBox::information(parent, "Import complete",
                             "Settings will be imported on the next JilChat "
                             "Chatterino start. Restart "
                             "JilChat Chatterino to load everything.");
}

}  // namespace

namespace chatterino {

SettingsProfilePage::SettingsProfilePage()
{
    auto *outer = new QVBoxLayout;
    auto *inner = new QHBoxLayout;
    auto *view = GeneralPageView::withoutNavigation(this);
    this->view_ = view;

    inner->addWidget(view);
    auto *frame = new QFrame;
    frame->setLayout(inner);
    outer->addWidget(frame);
    this->setLayout(outer);

    this->initLayout(*view);
}

bool SettingsProfilePage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }

    return false;
}

void SettingsProfilePage::initLayout(GeneralPageView &layout)
{
    layout.addTitle("Settings Import/Export");
    layout.addDescription(
        "Export or import all local settings, hotkeys, highlights, moderation "
        "buttons, commands, tabs, chat automations, user notes and colors, and "
        "your own themes with their backgrounds. Accounts are not exported.");
    {
        auto *box = new QHBoxLayout;
        box->addWidget(layout.makeButton("Export settings", [this]() {
            exportSettingsProfile(this);
        }));
        box->addWidget(layout.makeButton("Import settings", [this]() {
            importSettingsProfile(this);
        }));
        box->addStretch(1);

        layout.addLayout(box);
    }
}

}  // namespace chatterino
