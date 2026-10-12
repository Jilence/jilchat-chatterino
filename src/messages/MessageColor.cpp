// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageColor.hpp"

#include "messages/layouts/MessageLayoutContext.hpp"

namespace chatterino {

MessageColor::MessageColor(const QColor &color)
    : type_(Type::Custom)
    , customColor_(color)
{
}

MessageColor::MessageColor(Type type)
    : type_(type)
{
}

const QColor &MessageColor::getColor(const MessageColors &colors) const
{
    switch (this->type_)
    {
        case Type::Custom:
            return this->customColor_;
        case Type::Text:
            return colors.regularText;
        case Type::System:
            return colors.systemText;
        case Type::Link:
            return colors.linkText;
        case Type::Timestamp:
            return colors.timestampText;
    }

    static QColor _default;
    return _default;
}

QString MessageColor::toString() const
{
    switch (this->type_)
    {
        case Type::Custom:
            return this->customColor_.name(QColor::HexArgb);
        case Type::Text:
            return QStringLiteral("Text");
        case Type::System:
            return QStringLiteral("System");
        case Type::Link:
            return QStringLiteral("Link");
        case Type::Timestamp:
            // Timestamps used to be system colored. This name ends up in
            // the message snapshots of the tests, so it stays the same.
            return QStringLiteral("System");
        default:
            return {};
    }
}

QString MessageColor::toLua() const
{
    switch (this->type_)
    {
        case Type::Custom:
            return this->customColor_.name(QColor::HexArgb);
        case Type::Text:
            return QStringLiteral("text");
        case Type::System:
            return QStringLiteral("system");
        case Type::Link:
            return QStringLiteral("link");
        case Type::Timestamp:
            return QStringLiteral("timestamp");
        default:
            return {};
    }
}

MessageColor MessageColor::fromLua(const QString &spec, Type fallback)
{
    if (spec.isEmpty())
    {
        return fallback;
    }
    if (spec == u"text")
    {
        return MessageColor::Text;
    }
    if (spec == u"link")
    {
        return MessageColor::Link;
    }
    if (spec == u"system")
    {
        return MessageColor::System;
    }
    if (spec == u"timestamp")
    {
        return MessageColor::Timestamp;
    }

    return QColor(spec);
}

}  // namespace chatterino
