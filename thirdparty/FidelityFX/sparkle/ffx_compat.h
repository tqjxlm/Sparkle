// Force-included into the FidelityFX sources: the MSVC CRT functions and macros they use, for every other toolchain.
#pragma once

#define FFX_UNUSED(x) ((void)(x))

#if !defined(_WIN32)
#include <cstddef>
#include <cwchar>

#define _countof(array) (sizeof(array) / sizeof((array)[0]))

template <size_t N> int wcscpy_s(wchar_t (&destination)[N], const wchar_t *source)
{
    wcsncpy(destination, source, N - 1);
    destination[N - 1] = L'\0';
    return 0;
}
#endif
