// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QObject>
#include <QString>

#include <functional>

namespace chatterino {

/// Account secrets by key, kept with Credentials (the system keychain, or
/// the insecure file when that is turned on).
class AccountCredentials : public QObject
{
public:
    using ReadCallback = std::function<void(ExpectedStr<QString>)>;
    using WriteCallback = std::function<void(ExpectedStr<void>)>;

    /// A missing entry gives the error "Credential was not found."
    void read(const QString &key, ReadCallback callback);
    void write(const QString &key, const QString &value,
               WriteCallback callback);
    void remove(const QString &key, WriteCallback callback);
};

AccountCredentials &accountCredentials();

}  // namespace chatterino
