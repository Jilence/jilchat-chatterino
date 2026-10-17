// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Credentials.hpp"

#include "Application.hpp"
#include "common/Modes.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"
#include "util/Variant.hpp"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringBuilder>

#include <variant>

#ifndef NO_QTKEYCHAIN
#    ifdef CMAKE_BUILD
#        if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#            include "qt6keychain/keychain.h"
#        else
#            include "qt5keychain/keychain.h"
#        endif
#    else
#        include <qtkeychain/keychain.h>
#    endif
#endif

namespace {

using namespace chatterino;

QString formatName(const QString &provider, const QString &name)
{
    assert(!provider.contains(":"));
    return u"chatterino:" % provider % u':' % name;
}

bool useKeyring()
{
#ifdef NO_QTKEYCHAIN
    return false;
#endif
    Modes modes(getApp()->getArgs());
    if (modes.isPortable)
    {
        return false;
    }

#ifdef Q_OS_LINUX
    return getSettings()->useKeyring;
#else
    return true;
#endif
}

QString insecurePath()
{
    return combinePath(getApp()->getPaths().settingsDirectory,
                       "credentials.json");
}

QJsonDocument loadInsecure()
{
    QFile file(insecurePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    return QJsonDocument::fromJson(file.readAll());
}

bool storeInsecure(const QJsonDocument &doc)
{
    QSaveFile file(insecurePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }
    if (file.write(doc.toJson()) == -1)
    {
        return false;
    }
    return file.commit();
}

QString insecureSaveError()
{
    return QStringLiteral("Could not save credentials.");
}

QString credentialNotFoundError()
{
    return QStringLiteral("Credential was not found.");
}

QJsonDocument &insecureInstance()
{
    static auto store = loadInsecure();
    return store;
}

void queueInsecureSave()
{
    static bool isQueued = false;

    if (!isQueued)
    {
        isQueued = true;
        QTimer::singleShot(200, QApplication::instance(), [] {
            storeInsecure(insecureInstance());
            isQueued = false;
        });
    }
}

struct SetJob {
    QString name;
    QString credential;
    std::function<void(ExpectedStr<void>)> onFinished;
};

struct EraseJob {
    QString name;
    std::function<void(ExpectedStr<void>)> onFinished;
};

using Job = std::variant<SetJob, EraseJob>;

#ifndef NO_QTKEYCHAIN
QString keychainErrorText(const QKeychain::Job *job)
{
    const auto detail = job->errorString().trimmed();
    if (detail.isEmpty())
    {
        return QStringLiteral("The system credential store failed.");
    }
    return detail;
}

ExpectedStr<void> keychainWriteResult(const QKeychain::Job *job)
{
    if (job->error() == QKeychain::NoError)
    {
        return {};
    }
    return makeUnexpected(keychainErrorText(job));
}

ExpectedStr<void> keychainEraseResult(const QKeychain::Job *job)
{
    if (job->error() == QKeychain::NoError ||
        job->error() == QKeychain::EntryNotFound)
    {
        return {};
    }
    return makeUnexpected(keychainErrorText(job));
}

ExpectedStr<QString> keychainReadResult(const QKeychain::ReadPasswordJob *job)
{
    if (job->error() == QKeychain::EntryNotFound)
    {
        return makeUnexpected(credentialNotFoundError());
    }
    if (job->error() != QKeychain::NoError)
    {
        return makeUnexpected(keychainErrorText(job));
    }
    return job->textData();
}
#endif

void finishCredentialWrite(std::function<void(ExpectedStr<void>)> onFinished,
                           ExpectedStr<void> result)
{
    if (!onFinished)
    {
        return;
    }
    runInGuiThread([onFinished = std::move(onFinished),
                    result = std::move(result)]() mutable {
        onFinished(std::move(result));
    });
}

std::queue<Job> &jobQueue()
{
    static std::queue<Job> jobs;
    return jobs;
}

void runNextJob()
{
#ifndef NO_QTKEYCHAIN
    auto &&queue = jobQueue();

    if (!queue.empty())
    {
        auto &&item = queue.front();

        std::visit(
            variant::Overloaded{
                [](SetJob &set) {
                    auto *job = new QKeychain::WritePasswordJob("chatterino");
                    job->setAutoDelete(true);
                    job->setKey(set.name);
                    job->setTextData(set.credential);
                    QObject::connect(
                        job, &QKeychain::Job::finished,
                        QApplication::instance(),
                        [job, onFinished = std::move(set.onFinished)](auto) {
                            finishCredentialWrite(std::move(onFinished),
                                                  keychainWriteResult(job));
                            runNextJob();
                        });
                    job->start();
                },
                [](EraseJob &erase) {
                    auto *job = new QKeychain::DeletePasswordJob("chatterino");
                    job->setAutoDelete(true);
                    job->setKey(erase.name);
                    QObject::connect(
                        job, &QKeychain::Job::finished,
                        QApplication::instance(),
                        [job, onFinished = std::move(erase.onFinished)](auto) {
                            finishCredentialWrite(std::move(onFinished),
                                                  keychainEraseResult(job));
                            runNextJob();
                        });
                    job->start();
                },
            },
            item);

        queue.pop();
    }
#endif
}

void queueJob(Job &&job)
{
    auto &&queue = jobQueue();

    queue.push(std::move(job));
    if (queue.size() == 1)
    {
        runNextJob();
    }
}

}  // namespace

namespace chatterino {

Credentials &Credentials::instance()
{
    static Credentials creds;
    return creds;
}

void Credentials::get(const QString &provider, const QString &name_,
                      QObject *receiver,
                      std::function<void(const QString &)> &&onLoaded)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
#ifndef NO_QTKEYCHAIN

        auto *job = new QKeychain::ReadPasswordJob("chatterino");
        job->setAutoDelete(true);
        job->setKey(name);
        QObject::connect(
            job, &QKeychain::Job::finished, receiver,
            [job, onLoaded = std::move(onLoaded)](auto) mutable {
                onLoaded(job->textData());
            },
            Qt::DirectConnection);
        job->start();
#endif
    }
    else
    {
        const auto &instance = insecureInstance();

        onLoaded(instance[name].toString());
    }
}

void Credentials::get(const QString &provider, const QString &name_,
                      QObject *receiver,
                      std::function<void(ExpectedStr<QString>)> &&onLoaded)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
#ifndef NO_QTKEYCHAIN
        auto *job = new QKeychain::ReadPasswordJob("chatterino");
        job->setAutoDelete(true);
        job->setKey(name);
        QObject::connect(
            job, &QKeychain::Job::finished, receiver,
            [job, onLoaded = std::move(onLoaded)](auto) mutable {
                if (onLoaded)
                {
                    onLoaded(keychainReadResult(job));
                }
            },
            Qt::DirectConnection);
        job->start();
#endif
    }
    else if (onLoaded)
    {
        const auto object = insecureInstance().object();
        if (!object.contains(name))
        {
            onLoaded(makeUnexpected(credentialNotFoundError()));
        }
        else
        {
            onLoaded(object.value(name).toString());
        }
    }
}

void Credentials::set(const QString &provider, const QString &name_,
                      const QString &credential)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
        queueJob(SetJob{name, credential, {}});
    }
    else
    {
        auto &instance = insecureInstance();

        auto obj = instance.object();
        obj[name] = credential;
        instance.setObject(obj);

        queueInsecureSave();
    }
}

void Credentials::set(const QString &provider, const QString &name_,
                      const QString &credential,
                      std::function<void(ExpectedStr<void>)> &&onFinished)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
        queueJob(SetJob{name, credential, std::move(onFinished)});
        return;
    }

    auto &instance = insecureInstance();
    const auto previous = instance;
    auto obj = instance.object();
    obj[name] = credential;
    instance.setObject(obj);

    if (!storeInsecure(instance))
    {
        instance = previous;
        finishCredentialWrite(std::move(onFinished),
                              makeUnexpected(insecureSaveError()));
        return;
    }

    finishCredentialWrite(std::move(onFinished), {});
}

void Credentials::erase(const QString &provider, const QString &name_)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
        queueJob(EraseJob{name, {}});
    }
    else
    {
        auto &instance = insecureInstance();

        if (auto it = instance.object().find(name);
            it != instance.object().end())
        {
            instance.object().erase(it);
        }

        queueInsecureSave();
    }
}

void Credentials::erase(const QString &provider, const QString &name_,
                        std::function<void(ExpectedStr<void>)> &&onFinished)
{
    assertInGuiThread();

    auto name = formatName(provider, name_);

    if (useKeyring())
    {
        queueJob(EraseJob{name, std::move(onFinished)});
        return;
    }

    auto &instance = insecureInstance();
    auto obj = instance.object();
    if (!obj.contains(name))
    {
        finishCredentialWrite(std::move(onFinished), {});
        return;
    }

    const auto previous = instance;
    obj.remove(name);
    instance.setObject(obj);
    if (!storeInsecure(instance))
    {
        instance = previous;
        finishCredentialWrite(std::move(onFinished),
                              makeUnexpected(insecureSaveError()));
        return;
    }

    finishCredentialWrite(std::move(onFinished), {});
}

}  // namespace chatterino
