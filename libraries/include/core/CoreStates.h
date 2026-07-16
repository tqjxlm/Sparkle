#pragma once

#include <atomic>
#include <cstdint>

namespace sparkle
{
/*
    This class stores essential states of the program that can be accessed by all modules.
    It is designed to remove dependencies between state controllers and state observers.
*/
class CoreStates
{
public:
    enum AppState : uint8_t
    {
        Launch,
        Init,
        MainLoop,
        Exiting,
    };

    [[nodiscard]] AppState GetAppState() const
    {
        return app_state_.load(std::memory_order_acquire);
    }

    void SetAppState(AppState new_state)
    {
        app_state_.store(new_state, std::memory_order_release);
    }

    static CoreStates &Instance()
    {
        static CoreStates instance;
        return instance;
    }

    static bool IsExiting()
    {
        return Instance().GetAppState() == AppState::Exiting;
    }

private:
    std::atomic<AppState> app_state_{AppState::Launch};
};
} // namespace sparkle
