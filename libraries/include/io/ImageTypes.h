#pragma once

#include "core/Exception.h"
#include "core/math/Types.h"

namespace sparkle
{
enum class PixelFormat : uint8_t
{
    B8G8R8A8Srgb,
    B8G8R8A8Unorm,
    R8G8B8A8Srgb,
    R8G8B8A8Unorm,
    R32UInt,
    R32Float,
    D24S8,
    D32,
    RGBAFloat,
    RGBAFloat16,
    RGBAUInt32,
    R10G10B10A2Unorm,
    Count
};

constexpr unsigned GetFormatChannelCount(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::B8G8R8A8Srgb:
    case PixelFormat::B8G8R8A8Unorm:
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::R8G8B8A8Unorm:
    case PixelFormat::RGBAFloat:
    case PixelFormat::R10G10B10A2Unorm:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::RGBAUInt32:
        return 4;
    case PixelFormat::D24S8:
        return 2;
    case PixelFormat::D32:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
        return 1;
    case PixelFormat::Count:
    default:
        break;
    }
    UnImplemented(format);
    return 0;
}

constexpr unsigned GetPixelSize(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::B8G8R8A8Srgb:
    case PixelFormat::B8G8R8A8Unorm:
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::R8G8B8A8Unorm:
    case PixelFormat::R10G10B10A2Unorm:
    case PixelFormat::D24S8:
    case PixelFormat::D32:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
        return sizeof(uint32_t);
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAUInt32:
        return sizeof(uint32_t) * 4;
    case PixelFormat::RGBAFloat16:
        return sizeof(Half) * 4;
    case PixelFormat::Count:
    default:
        break;
    }
    UnImplemented(format);
    return 0;
}

constexpr bool IsSRGBFormat(PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::B8G8R8A8Srgb:
        return true;
    case PixelFormat::B8G8R8A8Unorm:
    case PixelFormat::R8G8B8A8Unorm:
    case PixelFormat::D24S8:
    case PixelFormat::D32:
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::R10G10B10A2Unorm:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
    case PixelFormat::RGBAUInt32:
        return false;
    case PixelFormat::Count:
    default:
        break;
    }
    UnImplemented(pixel_format);
    return false;
}

constexpr bool IsSwizzeldFormat(PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case PixelFormat::B8G8R8A8Srgb:
    case PixelFormat::B8G8R8A8Unorm:
        return true;
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::R8G8B8A8Unorm:
    case PixelFormat::D24S8:
    case PixelFormat::D32:
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::R10G10B10A2Unorm:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
    case PixelFormat::RGBAUInt32:
        return false;
    case PixelFormat::Count:
    default:
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
