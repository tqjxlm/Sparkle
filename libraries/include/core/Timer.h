#pragma once

#include <chrono>

namespace sparkle
{
class Timer
{
    using clock = std::chrono::high_resolution_clock;

public:
    Timer()
    {
        Reset();
    }

    void Reset()
    {
        start_point_ = clock::now();
    }

    [[nodiscard]] auto ElapsedNanoSecond() const
    {
        auto elapsed = clock::now() - start_point_;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    }

    [[nodiscard]] auto ElapsedMicroSecond() const
    {
        auto elapsed = clock::now() - start_point_;
        return std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    }

    [[nodiscard]] auto ElapsedMilliSecond() const
    {
        auto elapsed = clock::now() - start_point_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    }

    [[nodiscard]] float ElapsedSecond() const
    {
        return static_cast<float>(ElapsedMicroSecond()) * 1e-6f;
    }

private:
    clock::time_point start_point_;
};

class TimerCaller
{
public:
    TimerCaller(float interval_seconds, bool run_now, std::function<void(float)> &&func)
        : func_(std::move(func)), interval_(interval_seconds)
    {
        if (run_now)
        {
            func_(timer_.ElapsedSecond());
        }
        timer_.Reset();
    }

    void Tick()
    {
        if (timer_.ElapsedSecond() < interval_)
        {
            return;
        }

        func_(timer_.ElapsedSecond());

        timer_.Reset();
    }

private:
    std::function<void(float)> func_;
    float interval_;
    Timer timer_;
};

} // namespace sparkle
