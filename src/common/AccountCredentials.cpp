// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/AccountCredentials.hpp"

#include "common/Credentials.hpp"

namespace chatterino {

namespace {

const QString PROVIDER = QStringLiteral("accounts");

}  // namespace

void AccountCredentials::read(const QString &key, ReadCallback callback)
{
    std::function<void(ExpectedStr<QString>)> onLoaded =
        [callback = std::move(callback)](ExpectedStr<QString> result) {
            if (callback)
            {
                callback(std::move(result));
            }
        };
    Credentials::instance().get(PROVIDER, key, this, std::move(onLoaded));
}

void AccountCredentials::write(const QString &key, const QString &value,
                               WriteCallback callback)
{
    Credentials::instance().set(PROVIDER, key, value, std::move(callback));
}

void AccountCredentials::remove(const QString &key, WriteCallback callback)
{
    Credentials::instance().erase(PROVIDER, key, std::move(callback));
}

AccountCredentials &accountCredentials()
{
    static auto *credentials = new AccountCredentials;
    return *credentials;
}

}  // namespace chatterino
