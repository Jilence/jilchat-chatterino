// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// Included first in every C file of dav1d on Windows (see build.yml).
// dav1d 1.5.4's include/compat/msvc/stdatomic.h uses __has_include_next in an
// #if, which MSVC 19.5 (Visual Studio 2026) rejects with C7780 when the macro
// doesn't exist. dav1d's master branch defines this fallback in that header;
// remove this file once conan has a dav1d release with that fix.
#ifndef __has_include_next
#    define __has_include_next(x) 0
#endif
