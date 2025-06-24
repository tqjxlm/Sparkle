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

template <class T> class TimerCaller
{
public:
    TimerCaller(float interval_seconds, bool run_now, T &&func) : func_(std::move(func)), interval_(interval_seconds)
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
    T func_;
    float interval_;
    Timer timer_;
};

// to suppress -Wctad-maybe-unsupported
template <class T> TimerCaller(float, bool, T &&) -> TimerCaller<T>;

} // namespace sparkle
