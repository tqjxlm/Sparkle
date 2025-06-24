#pragma once

#include <cstdint>

namespace sparkle
{
/*
    This class stores essential states of the program that can be accessed by all modules.
    It is designed to remove dependencies between state controllers and state observers.
    TODO: make it thread-safe effienctly
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
        return app_state_;
    }

    void SetAppState(AppState new_state)
    {
        app_state_ = new_state;
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
    AppState app_state_ = AppState::Launch;
};
} // namespace sparkle
