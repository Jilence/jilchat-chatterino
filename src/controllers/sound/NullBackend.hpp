// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/sound/ISoundController.hpp"

namespace chatterino {

class NullBackend final : public ISoundController
{
public:
    using ISoundController::play;

    NullBackend();
    ~NullBackend() override = default;
    NullBackend(const NullBackend &) = delete;
    NullBackend(NullBackend &&) = delete;
    NullBackend &operator=(const NullBackend &) = delete;
    NullBackend &operator=(NullBackend &&) = delete;

    // Play a sound from the given url
    // If the url points to something that isn't a local file, it will play
    // the default sound initialized in the initialize method
    void play(const QUrl &sound, float volume) final;
};

}  // namespace chatterino
