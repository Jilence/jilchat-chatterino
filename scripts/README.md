<!--
SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>

SPDX-License-Identifier: CC0-1.0
-->

# scripts

This directory contains scripts that may be useful for a contributor to run while working on Chatterino

## Local Windows build (`dev-setup.ps1`, `dev-build.ps1`)

Builds and starts JilChat Chatterino locally without waiting for GitHub Actions.

Once: Visual Studio 2022 with the C++ workload, Python with Conan
(`python -m pip install --user conan`), LLVM (`winget install LLVM.LLVM`), then

    powershell -ExecutionPolicy Bypass -File scripts\dev-setup.ps1

installs Qt 6.9.3 to `C:\Qt`. After that:

    powershell -ExecutionPolicy Bypass -File scripts\dev-build.ps1 -Run

The first run builds the Conan dependencies (in a Conan home of its own under
`%LOCALAPPDATA%\leafyrino-dev`, so your Conan config isn't touched) and takes a
while; later runs only compile what changed. The app is
`build-dev\bin\JilChatChatterino.exe` and runs portable, with its settings in
`build-dev\bin` (`-SharedSettings` uses the normal settings folder instead).
`-Clean` starts over.
