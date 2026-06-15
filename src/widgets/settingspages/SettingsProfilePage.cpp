// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/SettingsProfilePage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QSaveFile>
#include <QVBoxLayout>

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
};

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
    getApp()->getCommands()->save();
    getApp()->getHotkeys()->save();
    getApp()->getWindows()->save();
    getSettings()->requestSave();

    const auto fileName = QFileDialog::getSaveFileName(
        parent, "Export Leafyrino settings",
        QDir::homePath() + "/leafyrino-settings.json",
        "Leafyrino settings (*.json);;JSON files (*.json)");
    if (fileName.isEmpty())
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
            QMessageBox::critical(parent, "Export failed",
                                  "Could not read " + relativeFile + ": " +
                                      error);
            return;
        }

        auto object = document.object();
        if (relativeFile == u"settings.json"_s)
        {
            object = sanitizeSettingsObject(object);
        }
        files.insert(relativeFile, object);
    }

    QJsonObject exportObject;
    exportObject.insert(u"format"_s, SETTINGS_EXPORT_FORMAT);
    exportObject.insert(u"version"_s, 1);
    exportObject.insert(u"files"_s, files);

    QString error;
    if (!writeJsonFile(fileName, QJsonDocument(exportObject), &error))
    {
        QMessageBox::critical(parent, "Export failed",
                              "Could not write export file: " + error);
        return;
    }

    QMessageBox::information(
        parent, "Export complete",
        "Settings were exported without Twitch/Kick accounts.");
}

void importSettingsProfile(QWidget *parent)
{
    const auto fileName = QFileDialog::getOpenFileName(
        parent, "Import Leafyrino settings", QDir::homePath(),
        "Leafyrino settings (*.json);;JSON files (*.json)");
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
        QMessageBox::critical(parent, "Import failed",
                              "This is not a Leafyrino settings export.");
        return;
    }

    auto reply = QMessageBox::question(
        parent, "Import settings",
        "Importing will replace local settings, hotkeys, highlights, "
        "moderation buttons, commands, and tab layout. Twitch/Kick accounts "
        "on this device will be kept.\n\nContinue?",
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

    QMessageBox::information(
        parent, "Import complete",
        "Settings will be imported on the next Leafyrino start. Restart "
        "Leafyrino to load everything.");
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
        "buttons, commands, and tabs. Accounts are not exported.");
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
