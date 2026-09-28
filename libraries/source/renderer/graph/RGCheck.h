#pragma once

#include "core/Exception.h"
#include "core/Logger.h"
#include "renderer/graph/RGError.h"

#include <format>

namespace sparkle
{
// graph declaration errors abort in every build: ASSERT compiles out of release builds. a test may make them throw
// (RGErrorsThrow).
template <typename... Args> void RGCheck(bool condition, std::format_string<Args...> format, Args &&...args)
{
    if (!condition)
    {
        auto message = std::format(format, std::forward<Args>(args)...);
#if ENABLE_TEST_CASES
        if (RGErrorsThrow::IsActive())
        {
            throw RGError(message);
        }
#endif
        Log(Error, "[RenderGraph] {}", message);
        DumpAndAbort();
    }
}
} // namespace sparkle
