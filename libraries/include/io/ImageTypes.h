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
    RG16Float,
    RG32Float,
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
    case PixelFormat::RG16Float:
    case PixelFormat::RG32Float:
        return 2;
    case PixelFormat::D32:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_FLOAT:
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
    case PixelFormat::RG32Float:
        return sizeof(float) * 2;
    case PixelFormat::RGBAFloat16:
        return sizeof(Half) * 4;
    case PixelFormat::RG16Float:
        return sizeof(Half) * 2;
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
    case PixelFormat::RG16Float:
    case PixelFormat::RG32Float:
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
    case PixelFormat::RG16Float:
    case PixelFormat::RG32Float:
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
    return format == PixelFormat::RGBAFloat || format == PixelFormat::RGBAFloat16 ||
           format == PixelFormat::RG16Float || format == PixelFormat::RG32Float;
}
} // namespace sparkle
