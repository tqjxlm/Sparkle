#include "io/TextureCompression.h"

#include "core/Exception.h"
#include "core/Logger.h"

#include <astcenc.h>
#include <bc7decomp.h>
#include <bc7enc.h>

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace sparkle
{
namespace
{
unsigned MipDim(unsigned base, unsigned level)
{
    return std::max(base >> level, 1u);
}

unsigned FullMipCount(unsigned width, unsigned height)
{
    unsigned count = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
        count++;
    }
    return count;
}

size_t MipChainByteSize(PixelFormat format, unsigned width, unsigned height, unsigned mip_count)
{
    size_t total = 0;
    for (auto mip = 0u; mip < mip_count; mip++)
    {
        total += GetImageMipByteSize(format, MipDim(width, mip), MipDim(height, mip));
    }
    return total;
}

// filtering must happen in linear space; sRGB content is linearized first
std::vector<float> ToLinearFloat(const uint8_t *rgba, unsigned width, unsigned height, bool srgb)
{
    std::vector<float> linear(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
    {
        Vector3 rgb{static_cast<float>(rgba[i * 4 + 0]) / 255.f, static_cast<float>(rgba[i * 4 + 1]) / 255.f,
                    static_cast<float>(rgba[i * 4 + 2]) / 255.f};
        if (srgb)
        {
            rgb = utilities::SRGBtoLinear(rgb);
        }
        linear[i * 4 + 0] = rgb.x();
        linear[i * 4 + 1] = rgb.y();
        linear[i * 4 + 2] = rgb.z();
        linear[i * 4 + 3] = static_cast<float>(rgba[i * 4 + 3]) / 255.f;
    }
    return linear;
}

std::vector<float> DownsampleBox(const std::vector<float> &source, unsigned source_width, unsigned source_height,
                                 unsigned dest_width, unsigned dest_height)
{
    std::vector<float> dest(static_cast<size_t>(dest_width) * dest_height * 4);
    for (unsigned y = 0; y < dest_height; y++)
    {
        const unsigned y0 = std::min(y * 2, source_height - 1);
        const unsigned y1 = std::min(y * 2 + 1, source_height - 1);
        for (unsigned x = 0; x < dest_width; x++)
        {
            const unsigned x0 = std::min(x * 2, source_width - 1);
            const unsigned x1 = std::min(x * 2 + 1, source_width - 1);
            for (unsigned channel = 0; channel < 4; channel++)
            {
                const float sum = source[(static_cast<size_t>(y0) * source_width + x0) * 4 + channel] +
                                  source[(static_cast<size_t>(y0) * source_width + x1) * 4 + channel] +
                                  source[(static_cast<size_t>(y1) * source_width + x0) * 4 + channel] +
                                  source[(static_cast<size_t>(y1) * source_width + x1) * 4 + channel];
                dest[(static_cast<size_t>(y) * dest_width + x) * 4 + channel] = sum * 0.25f;
            }
        }
    }
    return dest;
}

void RenormalizeTexels(std::vector<float> &texels)
{
    for (size_t i = 0; i < texels.size(); i += 4)
    {
        Vector3 normal{texels[i + 0] * 2.f - 1.f, texels[i + 1] * 2.f - 1.f, texels[i + 2] * 2.f - 1.f};
        const float length = normal.norm();
        if (length > 1e-6f)
        {
            normal /= length;
        }
        texels[i + 0] = normal.x() * 0.5f + 0.5f;
        texels[i + 1] = normal.y() * 0.5f + 0.5f;
        texels[i + 2] = normal.z() * 0.5f + 0.5f;
    }
}

uint8_t UnitFloatToUint8(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

std::vector<uint8_t> ToRgba8(const std::vector<float> &linear, bool srgb)
{
    std::vector<uint8_t> rgba(linear.size());
    for (size_t i = 0; i < linear.size(); i += 4)
    {
        Vector3 rgb{linear[i + 0], linear[i + 1], linear[i + 2]};
        if (srgb)
        {
            rgb = utilities::LinearToSrgb(rgb.cwiseMax(0.f).cwiseMin(1.f));
        }
        rgba[i + 0] = UnitFloatToUint8(rgb.x());
        rgba[i + 1] = UnitFloatToUint8(rgb.y());
        rgba[i + 2] = UnitFloatToUint8(rgb.z());
        rgba[i + 3] = UnitFloatToUint8(linear[i + 3]);
    }
    return rgba;
}

astcenc_profile GetAstcProfile(PixelFormat format)
{
    return IsSRGBFormat(format) ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
}

bool EncodeAstcMip(const uint8_t *rgba, unsigned width, unsigned height, astcenc_context *context, uint8_t *out,
                   size_t out_size)
{
    // the image is not modified, but the astcenc api takes mutable pointers
    void *slice = const_cast<uint8_t *>(rgba);
    astcenc_image image{.dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    const auto status = astcenc_compress_image(context, &image, &swizzle, out, out_size, 0);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc encode failed: {}", astcenc_get_error_string(status));
        return false;
    }
    return astcenc_compress_reset(context) == ASTCENC_SUCCESS;
}

void ExtractBlockRgba8(const uint8_t *rgba, unsigned width, unsigned height, unsigned block_x, unsigned block_y,
                       uint8_t *block_texels)
{
    for (unsigned y = 0; y < 4; y++)
    {
        const unsigned source_y = std::min(block_y * 4 + y, height - 1);
        for (unsigned x = 0; x < 4; x++)
        {
            const unsigned source_x = std::min(block_x * 4 + x, width - 1);
            memcpy(block_texels + (y * 4 + x) * 4, rgba + (static_cast<size_t>(source_y) * width + source_x) * 4, 4);
        }
    }
}

void EncodeBc7Mip(const uint8_t *rgba, unsigned width, unsigned height, const bc7enc_compress_block_params &params,
                  uint8_t *out)
{
    const unsigned blocks_x = (width + 3) / 4;
    const unsigned blocks_y = (height + 3) / 4;
    for (unsigned block_y = 0; block_y < blocks_y; block_y++)
    {
        for (unsigned block_x = 0; block_x < blocks_x; block_x++)
        {
            std::array<uint8_t, 64> block_texels;
            ExtractBlockRgba8(rgba, width, height, block_x, block_y, block_texels.data());
            bc7enc_compress_block(out + (static_cast<size_t>(block_y) * blocks_x + block_x) * 16, block_texels.data(),
                                  &params);
        }
    }
}

bool DecodeAstcMip(const uint8_t *blocks, size_t blocks_size, unsigned width, unsigned height, PixelFormat format,
                   uint8_t *out_rgba)
{
    astcenc_config config;
    const unsigned block_dim = GetBlockDim(format);
    auto status = astcenc_config_init(GetAstcProfile(format), block_dim, block_dim, 1, ASTCENC_PRE_MEDIUM,
                                      ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc decode config failed: {}", astcenc_get_error_string(status));
        return false;
    }

    astcenc_context *context = nullptr;
    status = astcenc_context_alloc(&config, 1, &context, nullptr);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc decode context failed: {}", astcenc_get_error_string(status));
        return false;
    }

    void *slice = out_rgba;
    astcenc_image image{.dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    status = astcenc_decompress_image(context, blocks, blocks_size, &image, &swizzle, 0);
    astcenc_context_free(context);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc decode failed: {}", astcenc_get_error_string(status));
        return false;
    }
    return true;
}

void DecodeBc7Mip(const uint8_t *blocks, unsigned width, unsigned height, uint8_t *out_rgba)
{
    const unsigned blocks_x = (width + 3) / 4;
    const unsigned blocks_y = (height + 3) / 4;
    for (unsigned block_y = 0; block_y < blocks_y; block_y++)
    {
        for (unsigned block_x = 0; block_x < blocks_x; block_x++)
        {
            std::array<bc7decomp::color_rgba, 16> texels;
            bc7decomp::unpack_bc7(blocks + (static_cast<size_t>(block_y) * blocks_x + block_x) * 16, texels.data());
            for (unsigned y = 0; y < 4; y++)
            {
                const unsigned dest_y = block_y * 4 + y;
                if (dest_y >= height)
                {
                    break;
                }
                for (unsigned x = 0; x < 4; x++)
                {
                    const unsigned dest_x = block_x * 4 + x;
                    if (dest_x >= width)
                    {
                        break;
                    }
                    memcpy(out_rgba + (static_cast<size_t>(dest_y) * width + dest_x) * 4, texels[y * 4 + x].m_comps, 4);
                }
            }
        }
    }
}

const bc7enc_compress_block_params &GetBc7Params(bool perceptual)
{
    static std::once_flag init_once;
    std::call_once(init_once, [] { bc7enc_compress_block_init(); });

    static const auto PerceptualParams = [] {
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        return params;
    }();
    static const auto LinearParams = [] {
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        bc7enc_compress_block_params_init_linear_weights(&params);
        return params;
    }();
    return perceptual ? PerceptualParams : LinearParams;
}
} // namespace

const char *TextureCompression::GetFamilyName(Family family)
{
    switch (family)
    {
    case Family::Astc:
        return "astc";
    case Family::Bc:
        return "bc";
    default:
        UnImplemented(family);
        return "";
    }
}

PixelFormat TextureCompression::SelectFormat(Profile profile, Family family)
{
    switch (family)
    {
    case Family::Astc:
        switch (profile)
        {
        case Profile::Color:
            return PixelFormat::ASTC6x6Srgb;
        case Profile::Data:
            return PixelFormat::ASTC6x6Unorm;
        case Profile::Normal:
            return PixelFormat::ASTC4x4Unorm;
        default:
            UnImplemented(profile);
            return PixelFormat::Count;
        }
    case Family::Bc:
        switch (profile)
        {
        case Profile::Color:
            return PixelFormat::BC7Srgb;
        case Profile::Data:
        case Profile::Normal:
            return PixelFormat::BC7Unorm;
        default:
            UnImplemented(profile);
            return PixelFormat::Count;
        }
    default:
        UnImplemented(family);
        return PixelFormat::Count;
    }
}

std::vector<char> TextureCompression::Encode(const Image2D &source, Profile profile, Family family)
{
    if (source.GetFormat() != PixelFormat::R8G8B8A8Srgb && source.GetFormat() != PixelFormat::R8G8B8A8Unorm)
    {
        Log(Error, "texture compression needs an RGBA8 source, got {} for {}", Enum2Str(source.GetFormat()),
            source.GetName());
        return {};
    }

    const PixelFormat target_format = SelectFormat(profile, family);
    const unsigned width = source.GetWidth();
    const unsigned height = source.GetHeight();
    const unsigned mip_count = FullMipCount(width, height);
    const bool srgb = profile == Profile::Color;

    std::vector<char> payload(sizeof(PayloadHeader) + MipChainByteSize(target_format, width, height, mip_count));
    const PayloadHeader header{
        .format = static_cast<uint32_t>(target_format), .width = width, .height = height, .mip_count = mip_count};
    memcpy(payload.data(), &header, sizeof(header));

    astcenc_context *astc_context = nullptr;
    if (family == Family::Astc)
    {
        astcenc_config config;
        const unsigned block_dim = GetBlockDim(target_format);
        auto status =
            astcenc_config_init(GetAstcProfile(target_format), block_dim, block_dim, 1, ASTCENC_PRE_MEDIUM, 0, &config);
        if (status == ASTCENC_SUCCESS)
        {
            status = astcenc_context_alloc(&config, 1, &astc_context, nullptr);
        }
        if (status != ASTCENC_SUCCESS)
        {
            Log(Error, "astc encoder setup failed: {}", astcenc_get_error_string(status));
            return {};
        }
    }

    bool success = true;
    size_t mip_offset = sizeof(PayloadHeader);
    std::vector<float> linear_mip;
    for (auto mip = 0u; mip < mip_count && success; mip++)
    {
        const unsigned mip_width = MipDim(width, mip);
        const unsigned mip_height = MipDim(height, mip);

        // mip 0 reuses the source bytes untouched; coarser mips are filtered in linear
        // space and re-quantized
        std::vector<uint8_t> filtered;
        const uint8_t *mip_rgba = nullptr;
        if (mip == 0)
        {
            mip_rgba = source.GetRawData();
        }
        else
        {
            if (mip == 1)
            {
                linear_mip = ToLinearFloat(source.GetRawData(), width, height, srgb);
            }
            linear_mip =
                DownsampleBox(linear_mip, MipDim(width, mip - 1), MipDim(height, mip - 1), mip_width, mip_height);
            if (profile == Profile::Normal)
            {
                RenormalizeTexels(linear_mip);
            }
            filtered = ToRgba8(linear_mip, srgb);
            mip_rgba = filtered.data();
        }

        const size_t mip_size = GetImageMipByteSize(target_format, mip_width, mip_height);
        auto *out = reinterpret_cast<uint8_t *>(payload.data()) + mip_offset;
        if (family == Family::Astc)
        {
            success = EncodeAstcMip(mip_rgba, mip_width, mip_height, astc_context, out, mip_size);
        }
        else
        {
            EncodeBc7Mip(mip_rgba, mip_width, mip_height, GetBc7Params(srgb), out);
        }
        mip_offset += mip_size;
    }

    if (astc_context != nullptr)
    {
        astcenc_context_free(astc_context);
    }

    return success ? payload : std::vector<char>{};
}

std::shared_ptr<Image2D> TextureCompression::CreateImageFromPayload(const std::vector<char> &payload,
                                                                    const std::string &name)
{
    if (payload.size() < sizeof(PayloadHeader))
    {
        Log(Error, "compressed texture payload for {} is too small", name);
        return nullptr;
    }

    PayloadHeader header;
    memcpy(&header, payload.data(), sizeof(header));

    const auto format = static_cast<PixelFormat>(header.format);
    if (header.format >= static_cast<uint32_t>(PixelFormat::Count) || !IsCompressedFormat(format) ||
        header.width == 0 || header.height == 0 || header.mip_count != FullMipCount(header.width, header.height))
    {
        Log(Error, "compressed texture payload for {} has a corrupt header", name);
        return nullptr;
    }

    const size_t chain_size = MipChainByteSize(format, header.width, header.height, header.mip_count);
    if (payload.size() != sizeof(PayloadHeader) + chain_size)
    {
        Log(Error, "compressed texture payload for {} has size {}, expected {}", name, payload.size(),
            sizeof(PayloadHeader) + chain_size);
        return nullptr;
    }

    std::vector<uint8_t> chain(chain_size);
    memcpy(chain.data(), payload.data() + sizeof(PayloadHeader), chain_size);

    return std::make_shared<Image2D>(header.width, header.height, format, header.mip_count, std::move(chain), name);
}

Image2D TextureCompression::Decode(const Image2D &compressed)
{
    const PixelFormat format = compressed.GetFormat();
    ASSERT(IsCompressedFormat(format));

    const unsigned width = compressed.GetWidth();
    const unsigned height = compressed.GetHeight();
    const auto decoded_format = IsSRGBFormat(format) ? PixelFormat::R8G8B8A8Srgb : PixelFormat::R8G8B8A8Unorm;

    const size_t mip0_size = GetImageMipByteSize(format, width, height);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    switch (format)
    {
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
        DecodeBc7Mip(compressed.GetRawData(), width, height, rgba.data());
        break;
    default:
        if (!DecodeAstcMip(compressed.GetRawData(), mip0_size, width, height, format, rgba.data()))
        {
            memset(rgba.data(), 0, rgba.size());
        }
        break;
    }

    Image2D decoded(width, height, decoded_format, rgba);
    decoded.SetName(compressed.GetName());
    return decoded;
}
} // namespace sparkle
