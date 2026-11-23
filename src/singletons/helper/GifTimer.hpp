// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QTimer>

#include <cstdint>
#include <memory>

namespace chatterino {

constexpr long unsigned GIF_FRAME_LENGTH = 20;

class GIFTimer
{
public:
    void initialize();

    pajlada::Signals::NoArgSignal signal;
    uint64_t position() const
    {
        return *this->position_;
    }

    std::shared_ptr<const uint64_t> clock() const
    {
        return this->position_;
    }

    void advance()
    {
        *this->position_ += GIF_FRAME_LENGTH;
        this->signal.invoke();
    }

    void registerOpenOverlayWindow()
    {
        this->openOverlayWindows_++;
        this->updateTimer();
    }

    void unregisterOpenOverlayWindow()
    {
        assert(this->openOverlayWindows_ >= 1);
        this->openOverlayWindows_--;
        this->updateTimer();
    }

private:
    void updateTimer();
    bool initialized_ = false;
    QTimer timer;
    std::shared_ptr<uint64_t> position_ = std::make_shared<uint64_t>(0);
    size_t openOverlayWindows_ = 0;
};

}  // namespace chatterino
