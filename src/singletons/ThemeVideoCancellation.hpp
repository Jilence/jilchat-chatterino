#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace chatterino {

/// Tells work on another thread that its result isn't wanted anymore.
///
/// Like std::stop_token, which libc++ only has behind
/// -fexperimental-library before LLVM 20.
class ThemeVideoStopToken
{
public:
    /// A token nobody can stop
    ThemeVideoStopToken() = default;

    bool stop_requested() const noexcept
    {
        return this->stopped_ && this->stopped_->load();
    }

private:
    explicit ThemeVideoStopToken(std::shared_ptr<std::atomic<bool>> stopped)
        : stopped_(std::move(stopped))
    {
    }

    std::shared_ptr<std::atomic<bool>> stopped_;

    friend class ThemeVideoStopSource;
};

class ThemeVideoStopSource
{
public:
    ThemeVideoStopSource()
        : stopped_(std::make_shared<std::atomic<bool>>(false))
    {
    }

    void request_stop() noexcept
    {
        this->stopped_->store(true);
    }

    ThemeVideoStopToken get_token() const
    {
        return ThemeVideoStopToken(this->stopped_);
    }

private:
    std::shared_ptr<std::atomic<bool>> stopped_;
};

}  // namespace chatterino
