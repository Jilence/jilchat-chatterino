// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QObject>
#include <QString>

#include <functional>

namespace chatterino {

class Credentials
{
public:
    static Credentials &instance();

    void get(const QString &provider, const QString &name, QObject *receiver,
             std::function<void(const QString &)> &&onLoaded);
    void get(const QString &provider, const QString &name, QObject *receiver,
             std::function<void(ExpectedStr<QString>)> &&onLoaded);
    void set(const QString &provider, const QString &name,
             const QString &credential);
    void set(const QString &provider, const QString &name,
             const QString &credential,
             std::function<void(ExpectedStr<void>)> &&onFinished);
    void erase(const QString &provider, const QString &name);
    void erase(const QString &provider, const QString &name,
               std::function<void(ExpectedStr<void>)> &&onFinished);

private:
    Credentials() = default;
};

}  // namespace chatterino
