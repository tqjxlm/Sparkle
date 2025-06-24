#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-macros"

#include "core/Enum.h"

#include <csignal>

namespace sparkle
{
inline void DumpAndAbort()
{
    // dumping is handled by ExceptionHandler via signal callback in debug build
    raise(SIGABRT);
}
} // namespace sparkle

#ifndef NDEBUG

#include "core/Logger.h"

namespace sparkle
{
class ExceptionHandler
{
public:
    ExceptionHandler();

    static void PrintStackTrace();

    static std::string GetStackTrace();

private:
    static bool instanced_;
};
} // namespace sparkle

#define ASSERT_F(condition, ...)                                                                                       \
    if (static_cast<bool>(condition))                                                                                  \
    {                                                                                                                  \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        Log(Error, __VA_ARGS__);                                                                                       \
                                                                                                                       \
        sparkle::DumpAndAbort();                                                                                       \
    }

#define ASSERT(condition)                                                                                              \
    if (static_cast<bool>(condition))                                                                                  \
    {                                                                                                                  \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        Log(Error, "[assertion failed] {}", #condition);                                                               \
                                                                                                                       \
        sparkle::DumpAndAbort();                                                                                       \
    }

#define ASSERT_EQUAL(expression1, expression2)                                                                         \
    {                                                                                                                  \
        auto evaluated1 = static_cast<int>(expression1);                                                               \
        auto evaluated2 = static_cast<int>(expression2);                                                               \
        if (evaluated1 == evaluated2)                                                                                  \
        {                                                                                                              \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            Log(Error, "[assertion failed] {} != {}", evaluated1, evaluated2);                                         \
                                                                                                                       \
            sparkle::DumpAndAbort();                                                                                   \
        }                                                                                                              \
    }

#else
#define ASSERT_F(condition, ...)                                                                                       \
    if (static_cast<bool>(condition))                                                                                  \
    {                                                                                                                  \
        ;                                                                                                              \
    }

#define ASSERT(condition)                                                                                              \
    if (static_cast<bool>(condition))                                                                                  \
    {                                                                                                                  \
        ;                                                                                                              \
    }

#define ASSERT_EQUAL(expression1, expression2)                                                                         \
    {                                                                                                                  \
        int evaluated1 = static_cast<int>(expression1);                                                                \
        int evaluated2 = static_cast<int>(expression2);                                                                \
        if (evaluated1 == evaluated2)                                                                                  \
        {                                                                                                              \
            ;                                                                                                          \
        }                                                                                                              \
    }
#endif

namespace sparkle
{
template <EnumType T> void UnImplemented([[maybe_unused]] T value)
{
    ASSERT_F(false, "unimplemented {}", sparkle::Enum2Str(value));
}

template <typename T> void UnImplemented([[maybe_unused]] T value)
{
    ASSERT_F(false, "unimplemented {}", value);
}

inline void UnImplemented()
{
    ASSERT_F(false, "unimplemented");
}

} // namespace sparkle

#pragma GCC diagnostic pop
