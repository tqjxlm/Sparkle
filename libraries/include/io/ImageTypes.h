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
    ASTC4x4Srgb,
    ASTC4x4Unorm,
    ASTC6x6Srgb,
    ASTC6x6Unorm,
    BC7Srgb,
    BC7Unorm,
    R16Float,
    RGFloat16,
    R9G9B9E5Float,
    ASTC6x6HDR,
    Count
};

// per-pixel accessors and GetPixelSize do not apply to block-compressed formats
constexpr bool IsCompressedFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
    case PixelFormat::ASTC6x6HDR:
        return true;
    default:
        return false;
    }
}

// texels per block edge
constexpr unsigned GetBlockDim(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
        return 4;
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::ASTC6x6HDR:
        return 6;
    default:
        UnImplemented(format);
        return 0;
    }
}

constexpr unsigned GetBlockByteSize(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
    case PixelFormat::ASTC6x6HDR:
        return 16;
    default:
        UnImplemented(format);
        return 0;
    }
}

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
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
    case PixelFormat::R9G9B9E5Float:
    case PixelFormat::ASTC6x6HDR:
        return 4;
    case PixelFormat::D24S8:
        return 2;
    case PixelFormat::D32:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
    case PixelFormat::R16Float:
        return 1;
    case PixelFormat::RGFloat16:
        return 2;
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
    case PixelFormat::R9G9B9E5Float:
    case PixelFormat::D24S8:
    case PixelFormat::D32:
    case PixelFormat::R32UInt:
    case PixelFormat::R32Float:
        return sizeof(uint32_t);
    case PixelFormat::R16Float:
        return sizeof(Half);
    case PixelFormat::RGFloat16:
        return sizeof(Half) * 2;
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

constexpr unsigned GetImageRowByteSize(PixelFormat format, unsigned width)
{
    if (IsCompressedFormat(format))
    {
        const unsigned block_dim = GetBlockDim(format);
        return (width + block_dim - 1) / block_dim * GetBlockByteSize(format);
    }
    return GetPixelSize(format) * width;
}

constexpr unsigned GetImageMipByteSize(PixelFormat format, unsigned width, unsigned height)
{
    if (IsCompressedFormat(format))
    {
        const unsigned block_dim = GetBlockDim(format);
        return GetImageRowByteSize(format, width) * ((height + block_dim - 1) / block_dim);
    }
    return GetImageRowByteSize(format, width) * height;
}

constexpr bool IsSRGBFormat(PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::B8G8R8A8Srgb:
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::BC7Srgb:
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
    case PixelFormat::R16Float:
    case PixelFormat::RGFloat16:
    case PixelFormat::RGBAUInt32:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::BC7Unorm:
    case PixelFormat::R9G9B9E5Float:
    case PixelFormat::ASTC6x6HDR:
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
    case PixelFormat::R16Float:
    case PixelFormat::RGFloat16:
    case PixelFormat::RGBAUInt32:
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
    case PixelFormat::R9G9B9E5Float:
    case PixelFormat::ASTC6x6HDR:
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
    switch (format)
    {
    case PixelFormat::RGBAFloat:
    case PixelFormat::RGBAFloat16:
    case PixelFormat::R9G9B9E5Float:
    case PixelFormat::ASTC6x6HDR:
        return true;
    default:
        return false;
    }
}

// HDR block-compressed formats decode to RGBAFloat16 rather than RGBA8
constexpr bool IsHDRCompressedFormat(PixelFormat format)
{
    return format == PixelFormat::ASTC6x6HDR;
}
} // namespace sparkle
