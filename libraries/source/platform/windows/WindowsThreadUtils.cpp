#if PLATFORM_WINDOWS

#include "core/ThreadUtils.h"

#include <Windows.h>

namespace sparkle
{

void SetCurrentThreadName(const std::string &name)
{
    // Windows 10, version 1607 and above
    // Convert UTF-8 std::string to UTF-16 std::wstring
    int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
        std::wstring wname(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
        SetThreadDescription(GetCurrentThread(), wname.c_str());
    }
}

} // namespace sparkle
#endif
