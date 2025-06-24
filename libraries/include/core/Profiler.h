#pragma once

#include "core/Logger.h"
#include "core/Timer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-macros"

#if ENABLE_PROFILER

#include <tracy/Tracy.hpp>

namespace sparkle
{
class Profiler
{
public:
    static void RegisterThreadForProfiling(const char *thread_name)
    {
        tracy::SetThreadName(thread_name);
    }
};
} // namespace sparkle

#define PROFILE_FRAME_START(name) FrameMarkStart(name)
#define PROFILE_FRAME_END(name) FrameMarkEnd(name)
#define PROFILE_SCOPE_AUTO ZoneScoped
#define PROFILE_SCOPE(name) ZoneScopedN(name)

#else

#define UNUSED(x) (void)(x);

#define PROFILE_FRAME_START(name) UNUSED(name)
#define PROFILE_FRAME_END(name) UNUSED(name)
#define PROFILE_SCOPE_AUTO
#define PROFILE_SCOPE(name) UNUSED(name)

#endif

namespace sparkle
{

// a logger that print elapsed time when destroyed
class ScopedTimeLogger
{
public:
    explicit ScopedTimeLogger(std::string_view name) : name_(name)
    {
    }

    ~ScopedTimeLogger()
    {
        Log(Info, "{} took {} seconds", name_, timer_.ElapsedSecond());
    }

private:
    Timer timer_;

    // it is safe to use a string_view since we only allow this class to be allocated on stack.
    // in that case, the string will always live longer than this class.
    std::string_view name_;
};
} // namespace sparkle

#define PROFILE_SCOPE_LOG(name)                                                                                        \
    ScopedTimeLogger logger(name);                                                                                     \
    PROFILE_SCOPE(name)

#pragma GCC diagnostic pop
