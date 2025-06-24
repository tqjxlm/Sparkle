#pragma once

#include <string>

namespace sparkle
{
// Sets the OS-level name of the current thread for debugger visibility.
// Name length limits: Linux (16), macOS/iOS (64), Windows (no practical limit).
void SetCurrentThreadName(const std::string &name);
} // namespace sparkle
