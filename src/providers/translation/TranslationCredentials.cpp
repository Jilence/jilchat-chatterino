#include "common/Credentials.hpp"
#include "providers/translation/Translator.hpp"
#include "util/PostToThread.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <utility>

namespace chatterino {

namespace {

QHash<int, QString> credentialCache;
QSet<int> loadedCredentials;
QHash<int, quint64> credentialRevisions;
QHash<int, std::vector<TranslationCredentialCallback>> pendingReads;

int cacheKey(TranslationProvider provider)
{
    return static_cast<int>(provider);
}

}  // namespace

QString translationProviderCredentialKey(TranslationProvider provider)
{
    switch (provider)
    {
        case TranslationProvider::DeepL:
            return QStringLiteral("deepl");
        case TranslationProvider::LibreTranslate:
            return QStringLiteral("libretranslate");
        case TranslationProvider::GoogleCloud:
            return QStringLiteral("google-cloud");
        case TranslationProvider::MicrosoftAzure:
            return QStringLiteral("azure");
        case TranslationProvider::GoogleBuiltIn:
        case TranslationProvider::MicrosoftFree:
            return {};
    }
    return {};
}

void readTranslationProviderCredential(TranslationProvider provider,
                                       TranslationCredentialCallback callback,
                                       bool refresh)
{
    const auto credentialKey = translationProviderCredentialKey(provider);
    if (credentialKey.isEmpty())
    {
        if (callback)
        {
            callback(QString{});
        }
        return;
    }

    const auto key = cacheKey(provider);
    if (!refresh && loadedCredentials.contains(key))
    {
        const auto cached = credentialCache.value(key);
        if (callback)
        {
            callback(cached);
        }
        return;
    }

    if (pendingReads.contains(key))
    {
        pendingReads[key].push_back(std::move(callback));
        return;
    }
    pendingReads[key].push_back(std::move(callback));
    const auto revision = credentialRevisions.value(key);
    Credentials::instance().get(
        QStringLiteral("translation"), credentialKey,
        QCoreApplication::instance(),
        std::function<void(ExpectedStr<QString>)>{
            [key, revision](ExpectedStr<QString> credential) mutable {
                runInGuiThread([key, revision,
                                credential = std::move(credential)]() mutable {
                    if (credentialRevisions.value(key) != revision)
                    {
                        credential = credentialCache.value(key);
                    }
                    const bool missing =
                        !credential &&
                        credential.error() ==
                            QStringLiteral("Credential was not found.");
                    if (credential && !credential->trimmed().isEmpty())
                    {
                        loadedCredentials.insert(key);
                        credentialCache.insert(key, credential->trimmed());
                    }
                    else if (credential || missing)
                    {
                        loadedCredentials.insert(key);
                        credentialCache.insert(key, QString{});
                    }
                    const auto callbacks = pendingReads.take(key);
                    for (const auto &pending : callbacks)
                    {
                        if (pending)
                        {
                            pending(missing ? ExpectedStr<QString>(QString{})
                                            : credential);
                        }
                    }
                });
            }});
}

void writeTranslationProviderCredential(
    TranslationProvider provider, const QString &credential,
    TranslationCredentialWriteCallback callback)
{
    const auto credentialKey = translationProviderCredentialKey(provider);
    const auto normalized = credential.trimmed();
    if (credentialKey.isEmpty() || normalized.isEmpty())
    {
        if (callback)
        {
            callback(makeUnexpected(
                QStringLiteral("Enter an API key before saving.")));
        }
        return;
    }

    Credentials::instance().set(
        QStringLiteral("translation"), credentialKey, normalized,
        [provider, normalized,
         callback = std::move(callback)](ExpectedStr<void> result) mutable {
            runInGuiThread([provider, normalized,
                            callback = std::move(callback),
                            result = std::move(result)]() mutable {
                if (result)
                {
                    const auto key = cacheKey(provider);
                    ++credentialRevisions[key];
                    loadedCredentials.insert(key);
                    credentialCache.insert(key, normalized);
                }
                if (callback)
                {
                    callback(std::move(result));
                }
            });
        });
}

void removeTranslationProviderCredential(
    TranslationProvider provider, TranslationCredentialWriteCallback callback)
{
    const auto credentialKey = translationProviderCredentialKey(provider);
    if (credentialKey.isEmpty())
    {
        if (callback)
        {
            callback(ExpectedStr<void>{});
        }
        return;
    }

    Credentials::instance().erase(
        QStringLiteral("translation"), credentialKey,
        [provider,
         callback = std::move(callback)](ExpectedStr<void> result) mutable {
            runInGuiThread([provider, callback = std::move(callback),
                            result = std::move(result)]() mutable {
                if (result)
                {
                    const auto key = cacheKey(provider);
                    ++credentialRevisions[key];
                    loadedCredentials.insert(key);
                    credentialCache.insert(key, QString{});
                }
                if (callback)
                {
                    callback(std::move(result));
                }
            });
        });
}

}  // namespace chatterino
