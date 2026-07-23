#include "io/TextureCompression.h"

#include "core/Exception.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"

#include <astcenc.h>
#include <bc7decomp.h>
#include <bc7enc.h>

#include <array>
#include <atomic>
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
    // Scale source-pixel boundaries by the destination dimensions so every overlap
    // weight is integral; each destination footprint then has this constant area.
    const double footprint_area = static_cast<double>(source_width) * source_height;
    for (unsigned y = 0; y < dest_height; y++)
    {
        const uint64_t y_begin = static_cast<uint64_t>(y) * source_height;
        const uint64_t y_end = static_cast<uint64_t>(y + 1) * source_height;
        const auto source_y_begin = static_cast<unsigned>(y_begin / dest_height);
        const unsigned source_y_end =
            std::min(static_cast<unsigned>((y_end + dest_height - 1) / dest_height), source_height);
        for (unsigned x = 0; x < dest_width; x++)
        {
            const uint64_t x_begin = static_cast<uint64_t>(x) * source_width;
            const uint64_t x_end = static_cast<uint64_t>(x + 1) * source_width;
            const auto source_x_begin = static_cast<unsigned>(x_begin / dest_width);
            const unsigned source_x_end =
                std::min(static_cast<unsigned>((x_end + dest_width - 1) / dest_width), source_width);
            std::array<double, 4> sum{};

            for (unsigned source_y = source_y_begin; source_y < source_y_end; source_y++)
            {
                const uint64_t source_y_begin_scaled = static_cast<uint64_t>(source_y) * dest_height;
                const uint64_t source_y_end_scaled = static_cast<uint64_t>(source_y + 1) * dest_height;
                const uint64_t y_weight =
                    std::min(y_end, source_y_end_scaled) - std::max(y_begin, source_y_begin_scaled);
                for (unsigned source_x = source_x_begin; source_x < source_x_end; source_x++)
                {
                    const uint64_t source_x_begin_scaled = static_cast<uint64_t>(source_x) * dest_width;
                    const uint64_t source_x_end_scaled = static_cast<uint64_t>(source_x + 1) * dest_width;
                    const uint64_t x_weight =
                        std::min(x_end, source_x_end_scaled) - std::max(x_begin, source_x_begin_scaled);
                    const double weight = static_cast<double>(x_weight) * static_cast<double>(y_weight);
                    const size_t source_offset = (static_cast<size_t>(source_y) * source_width + source_x) * 4;
                    for (unsigned channel = 0; channel < 4; channel++)
                    {
                        sum[channel] += static_cast<double>(source[source_offset + channel]) * weight;
                    }
                }
            }

            const size_t dest_offset = (static_cast<size_t>(y) * dest_width + x) * 4;
            for (unsigned channel = 0; channel < 4; channel++)
            {
                dest[dest_offset + channel] = static_cast<float>(sum[channel] / footprint_area);
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
    if (IsHDRCompressedFormat(format))
    {
        return ASTCENC_PRF_HDR;
    }
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

bool EncodeAstcHdrMip(const Half *fp16, unsigned width, unsigned height, astcenc_context *context, uint8_t *out,
                      size_t out_size)
{
    void *slice = const_cast<Half *>(fp16);
    astcenc_image image{.dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_F16, .data = &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    const auto status = astcenc_compress_image(context, &image, &swizzle, out, out_size, 0);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc hdr encode failed: {}", astcenc_get_error_string(status));
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

bool DecodeAstcHdrMip(const uint8_t *blocks, size_t blocks_size, unsigned width, unsigned height, PixelFormat format,
                      Half *out_fp16)
{
    astcenc_config config;
    const unsigned block_dim = GetBlockDim(format);
    auto status = astcenc_config_init(GetAstcProfile(format), block_dim, block_dim, 1, ASTCENC_PRE_MEDIUM,
                                      ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc hdr decode config failed: {}", astcenc_get_error_string(status));
        return false;
    }

    astcenc_context *context = nullptr;
    status = astcenc_context_alloc(&config, 1, &context, nullptr);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc hdr decode context failed: {}", astcenc_get_error_string(status));
        return false;
    }

    void *slice = out_fp16;
    astcenc_image image{.dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_F16, .data = &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    status = astcenc_decompress_image(context, blocks, blocks_size, &image, &swizzle, 0);
    astcenc_context_free(context);
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc hdr decode failed: {}", astcenc_get_error_string(status));
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

PixelFormat TextureCompression::SelectHdrFormat(Family family)
{
    switch (family)
    {
    case Family::Astc:
        return PixelFormat::ASTC6x6HDR;
    case Family::Bc:
        return PixelFormat::R9G9B9E5Float;
    }
    UnImplemented(family);
    return PixelFormat::Count;
}

TextureCompression::Family TextureCompression::FamilyFromHdrFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::ASTC4x4HDR:
    case PixelFormat::ASTC6x6HDR:
        return Family::Astc;
    case PixelFormat::R9G9B9E5Float:
        return Family::Bc;
    default:
        UnImplemented(format);
        return Family::Bc;
    }
}

std::vector<uint8_t> TextureCompression::EncodeHdrFace(const Image2D &source, PixelFormat target_format)
{
    ASSERT(source.GetFormat() == PixelFormat::RGBAFloat16);

    const unsigned width = source.GetWidth();
    const unsigned height = source.GetHeight();

    if (target_format == PixelFormat::R9G9B9E5Float)
    {
        Image2D packed(width, height, PixelFormat::R9G9B9E5Float);
        for (unsigned y = 0; y < height; y++)
        {
            for (unsigned x = 0; x < width; x++)
            {
                packed.SetPixel(x, y, Vector3(source.AccessPixel(x, y).head<3>()));
            }
        }
        std::vector<uint8_t> bytes(packed.GetStorageSize());
        memcpy(bytes.data(), packed.GetRawData(), bytes.size());
        return bytes;
    }

    ASSERT(IsHDRCompressedFormat(target_format));

    astcenc_config config;
    const unsigned block_dim = GetBlockDim(target_format);
    auto status =
        astcenc_config_init(GetAstcProfile(target_format), block_dim, block_dim, 1, ASTCENC_PRE_MEDIUM, 0, &config);
    astcenc_context *context = nullptr;
    if (status == ASTCENC_SUCCESS)
    {
        status = astcenc_context_alloc(&config, 1, &context, nullptr);
    }
    if (status != ASTCENC_SUCCESS)
    {
        Log(Error, "astc hdr encoder setup failed: {}", astcenc_get_error_string(status));
        return {};
    }

    std::vector<uint8_t> blocks(GetImageMipByteSize(target_format, width, height));
    const bool ok = EncodeAstcHdrMip(reinterpret_cast<const Half *>(source.GetRawData()), width, height, context,
                                     blocks.data(), blocks.size());
    astcenc_context_free(context);
    return ok ? blocks : std::vector<uint8_t>{};
}

std::vector<char> TextureCompression::EncodeHdrCube(const uint8_t *fp16, unsigned width, unsigned height,
                                                    unsigned mip_count, PixelFormat target_format)
{
    struct FaceJob
    {
        size_t in_offset;
        size_t out_offset;
        unsigned width;
        unsigned height;
    };

    std::vector<FaceJob> face_jobs;
    size_t in_offset = 0;
    size_t out_offset = sizeof(PayloadHeader);
    for (unsigned mip = 0; mip < mip_count; mip++)
    {
        const unsigned mip_width = MipDim(width, mip);
        const unsigned mip_height = MipDim(height, mip);
        const size_t fp16_face_size =
            static_cast<size_t>(mip_width) * mip_height * GetPixelSize(PixelFormat::RGBAFloat16);
        const size_t compressed_face_size = GetImageMipByteSize(target_format, mip_width, mip_height);
        for (unsigned face = 0; face < 6; face++)
        {
            face_jobs.push_back({in_offset, out_offset, mip_width, mip_height});
            in_offset += fp16_face_size;
            out_offset += compressed_face_size;
        }
    }

    std::vector<char> payload(out_offset);
    const PayloadHeader header{
        .format = static_cast<uint32_t>(target_format), .width = width, .height = height, .mip_count = mip_count};
    std::memcpy(payload.data(), &header, sizeof(header));

    std::atomic<bool> success{true};
    TaskManager::ParallelFor(0u, static_cast<unsigned>(face_jobs.size()), [&](unsigned index) {
        const auto &job = face_jobs[index];
        const size_t fp16_face_size =
            static_cast<size_t>(job.width) * job.height * GetPixelSize(PixelFormat::RGBAFloat16);
        const Image2D fp16_face(job.width, job.height, PixelFormat::RGBAFloat16,
                                std::vector<uint8_t>(fp16 + job.in_offset, fp16 + job.in_offset + fp16_face_size));
        const auto encoded = EncodeHdrFace(fp16_face, target_format);
        if (encoded.size() != GetImageMipByteSize(target_format, job.width, job.height))
        {
            success.store(false);
            return;
        }
        std::memcpy(payload.data() + job.out_offset, encoded.data(), encoded.size());
    }).wait();

    return success.load() ? payload : std::vector<char>{};
}

std::vector<uint8_t> TextureCompression::DecodeHdrCube(const std::vector<char> &payload)
{
    if (payload.size() < sizeof(PayloadHeader))
    {
        return {};
    }
    PayloadHeader header;
    std::memcpy(&header, payload.data(), sizeof(header));
    const auto format = static_cast<PixelFormat>(header.format);
    if (header.format >= static_cast<uint32_t>(PixelFormat::Count) || !IsHDRFormat(format))
    {
        return {};
    }

    size_t fp16_total = 0;
    size_t compressed_total = 0;
    for (unsigned mip = 0; mip < header.mip_count; mip++)
    {
        const unsigned mip_width = MipDim(header.width, mip);
        const unsigned mip_height = MipDim(header.height, mip);
        fp16_total += 6 * static_cast<size_t>(mip_width) * mip_height * GetPixelSize(PixelFormat::RGBAFloat16);
        compressed_total += 6 * GetImageMipByteSize(format, mip_width, mip_height);
    }
    if (payload.size() != sizeof(PayloadHeader) + compressed_total)
    {
        return {};
    }

    std::vector<uint8_t> fp16(fp16_total);
    size_t in_offset = sizeof(PayloadHeader);
    size_t out_offset = 0;
    for (unsigned mip = 0; mip < header.mip_count; mip++)
    {
        const unsigned mip_width = MipDim(header.width, mip);
        const unsigned mip_height = MipDim(header.height, mip);
        const size_t fp16_face_size =
            static_cast<size_t>(mip_width) * mip_height * GetPixelSize(PixelFormat::RGBAFloat16);
        const size_t compressed_face_size = GetImageMipByteSize(format, mip_width, mip_height);
        for (unsigned face = 0; face < 6; face++)
        {
            std::vector<uint8_t> blocks(payload.begin() + static_cast<std::ptrdiff_t>(in_offset),
                                        payload.begin() + static_cast<std::ptrdiff_t>(in_offset + compressed_face_size));
            Image2D fp16_face(mip_width, mip_height, PixelFormat::RGBAFloat16);
            if (IsCompressedFormat(format))
            {
                const Image2D compressed(mip_width, mip_height, format, 1, std::move(blocks), "ibl_cube");
                fp16_face = Decode(compressed, 0);
            }
            else
            {
                const Image2D packed(mip_width, mip_height, format, blocks);
                fp16_face.CopyFrom(packed);
            }
            std::memcpy(fp16.data() + out_offset, fp16_face.GetRawData(), fp16_face_size);
            in_offset += compressed_face_size;
            out_offset += fp16_face_size;
        }
    }
    return fp16;
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

Image2D TextureCompression::Decode(const Image2D &compressed, unsigned mip_level)
{
    const PixelFormat format = compressed.GetFormat();
    ASSERT(IsCompressedFormat(format));
    ASSERT(mip_level < compressed.GetMipCount());

    const unsigned width = MipDim(compressed.GetWidth(), mip_level);
    const unsigned height = MipDim(compressed.GetHeight(), mip_level);
    const auto decoded_format = IsSRGBFormat(format) ? PixelFormat::R8G8B8A8Srgb : PixelFormat::R8G8B8A8Unorm;

    size_t mip_offset = 0;
    for (auto mip = 0u; mip < mip_level; mip++)
    {
        mip_offset +=
            GetImageMipByteSize(format, MipDim(compressed.GetWidth(), mip), MipDim(compressed.GetHeight(), mip));
    }
    const size_t mip_size = GetImageMipByteSize(format, width, height);
    const auto *mip_data = compressed.GetRawData() + mip_offset;

    if (IsHDRCompressedFormat(format))
    {
        std::vector<uint8_t> fp16(static_cast<size_t>(width) * height * GetPixelSize(PixelFormat::RGBAFloat16));
        if (!DecodeAstcHdrMip(mip_data, mip_size, width, height, format, reinterpret_cast<Half *>(fp16.data())))
        {
            memset(fp16.data(), 0, fp16.size());
        }
        Image2D decoded_hdr(width, height, PixelFormat::RGBAFloat16, fp16);
        decoded_hdr.SetName(compressed.GetName());
        return decoded_hdr;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    switch (format)
    {
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
        DecodeBc7Mip(mip_data, width, height, rgba.data());
        break;
    default:
        if (!DecodeAstcMip(mip_data, mip_size, width, height, format, rgba.data()))
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
