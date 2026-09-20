// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <variant>

namespace chatterino::variant {

template <class... Ts>
struct [[nodiscard]] Overloaded : Ts... {
    using Ts::operator()...;

    constexpr decltype(auto) visit(auto &&v) &
    {
        return std::visit(*this, std::forward<decltype(v)>(v));
    }

    constexpr decltype(auto) visit(auto &&v) const &
    {
        return std::visit(*this, std::forward<decltype(v)>(v));
    }

    constexpr decltype(auto) visit(auto &&v) &&
    {
        return std::visit(std::move(*this), std::forward<decltype(v)>(v));
    }

    constexpr decltype(auto) visit(auto &&v) const &&
    {
        return std::visit(std::move(*this), std::forward<decltype(v)>(v));
    }
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace chatterino::variant
