#pragma once

#include "core/Exception.h"
#include "core/math/Types.h"

namespace sparkle
{
enum class PixelFormat : uint8_t
{
    B8G8R8A8_SRGB,
    B8G8R8A8_UNORM,
    R8G8B8A8_SRGB,
    R8G8B8A8_UNORM,
    R32_UINT,
    R32_FLOAT,
    D24_S8,
    D32,
    RGBAFloat,
    RGBAFloat16,
    RGBAUInt32,
    R10G10B10A2_UNORM,
    Count
};

constexpr unsigned GetFormatChannelCount(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::R8G8B8A8_SRGB:
    case PixelFormat::R8G8B8A8_UNORM:
    case PixelFormat::RGBAFloat:
    case PixelFormat::R10G10B10A2_UNORM:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::RGBAUInt32:
        return 4;
    case PixelFormat::D24_S8:
        return 2;
    case PixelFormat::D32:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_FLOAT:
        return 1;
    case PixelFormat::Count:
        break;
    }
    UnImplemented(format);
    return 0;
}

constexpr unsigned GetPixelSize(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::R8G8B8A8_SRGB:
    case PixelFormat::R8G8B8A8_UNORM:
    case PixelFormat::R10G10B10A2_UNORM:
    case PixelFormat::D24_S8:
    case PixelFormat::D32:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_FLOAT:
        return sizeof(uint32_t);
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAUInt32:
        return sizeof(uint32_t) * 4;
    case PixelFormat::RGBAFloat16:
        return sizeof(Half) * 4;
    case PixelFormat::Count:
        break;
    }
    UnImplemented(format);
    return 0;
}

constexpr bool IsSRGBFormat(PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case PixelFormat::R8G8B8A8_SRGB:
    case PixelFormat::B8G8R8A8_SRGB:
        return true;
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::R8G8B8A8_UNORM:
    case PixelFormat::D24_S8:
    case PixelFormat::D32:
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::R10G10B10A2_UNORM:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::RGBAUInt32:
        return false;
    case PixelFormat::Count:
        break;
    }
    UnImplemented(pixel_format);
    return false;
}

constexpr bool IsSwizzeldFormat(PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
        return true;
    case PixelFormat::R8G8B8A8_SRGB:
    case PixelFormat::R8G8B8A8_UNORM:
    case PixelFormat::D24_S8:
    case PixelFormat::D32:
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::R10G10B10A2_UNORM:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::RGBAUInt32:
        return false;
    case PixelFormat::Count:
        break;
    }
    UnImplemented(pixel_format);
    return false;
}

constexpr bool IsHDRFormat(PixelFormat format)
{
    return format == PixelFormat::RGBAFloat || format == PixelFormat::RGBAFloat16;
}
} // namespace sparkle
