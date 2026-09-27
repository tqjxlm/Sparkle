#pragma once

#include "core/Exception.h"
#include "core/Logger.h"

#include <format>

namespace sparkle
{
// graph declaration errors abort in every build: ASSERT compiles out of release builds
template <typename... Args> void RGCheck(bool condition, std::format_string<Args...> format, Args &&...args)
{
    if (!condition)
    {
        Log(Error, "[RenderGraph] {}", std::format(format, std::forward<Args>(args)...));
        DumpAndAbort();
    }
}
} // namespace sparkle
