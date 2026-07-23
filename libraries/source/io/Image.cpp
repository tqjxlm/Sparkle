#include "io/Image.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "io/ImageTypes.h"
#include "io/TextureCompression.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#pragma clang diagnostic pop

#include <crc32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>

namespace sparkle
{
namespace
{
uint8_t FloatToUint8(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

void ConvertPixelsToRGBA8(const uint8_t *source_data, uint8_t *dest, uint32_t pixel_count, PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::R8G8B8A8Srgb:
    case PixelFormat::R8G8B8A8Unorm:
        memcpy(dest, source_data, pixel_count * 4);
        break;
    case PixelFormat::B8G8R8A8Srgb:
    case PixelFormat::B8G8R8A8Unorm:
        for (uint32_t i = 0; i < pixel_count; i++)
        {
            dest[i * 4 + 0] = source_data[i * 4 + 2];
            dest[i * 4 + 1] = source_data[i * 4 + 1];
            dest[i * 4 + 2] = source_data[i * 4 + 0];
            dest[i * 4 + 3] = source_data[i * 4 + 3];
        }
        break;
    case PixelFormat::RGBAFloat16: {
        const auto *src = reinterpret_cast<const Half *>(source_data);
        for (uint32_t i = 0; i < pixel_count; i++)
        {
            Vector3 rgb{static_cast<float>(src[i * 4 + 0]), static_cast<float>(src[i * 4 + 1]),
                        static_cast<float>(src[i * 4 + 2])};
            auto srgb = utilities::LinearToSrgb(rgb.cwiseMax(0.f).cwiseMin(1.f));
            dest[i * 4 + 0] = FloatToUint8(srgb.x());
            dest[i * 4 + 1] = FloatToUint8(srgb.y());
            dest[i * 4 + 2] = FloatToUint8(srgb.z());
            dest[i * 4 + 3] = FloatToUint8(static_cast<float>(src[i * 4 + 3]));
        }
        break;
    }
    case PixelFormat::RGBAFloat: {
        const auto *src = reinterpret_cast<const float *>(source_data);
        for (uint32_t i = 0; i < pixel_count; i++)
        {
            Vector3 rgb{src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]};
            auto srgb = utilities::LinearToSrgb(rgb.cwiseMax(0.f).cwiseMin(1.f));
            dest[i * 4 + 0] = FloatToUint8(srgb.x());
            dest[i * 4 + 1] = FloatToUint8(srgb.y());
            dest[i * 4 + 2] = FloatToUint8(srgb.z());
            dest[i * 4 + 3] = FloatToUint8(src[i * 4 + 3]);
        }
        break;
    }
    default:
        Log(Error, "Unsupported pixel format for RGBA8 conversion: {}", Enum2Str(format));
        memset(dest, 0, pixel_count * 4);
        break;
    }
}

template <typename T> auto OwnStbiPixels(T *pixels)
{
    return std::unique_ptr<T, decltype(&stbi_image_free)>(pixels, stbi_image_free);
}

uint32_t FinishContentHash(CRC32 &hasher, uint32_t width, uint32_t height, PixelFormat format)
{
    const std::array<uint32_t, 3> meta{width, height, static_cast<uint32_t>(format)};
    hasher.add(meta.data(), meta.size() * sizeof(uint32_t));

    uint32_t hash = 0;
    hasher.getHash(reinterpret_cast<unsigned char *>(&hash));
    return hash;
}
} // namespace

Vector4 Image2D::UnpackRGB9E5(uint32_t packed)
{
    constexpr int MantissaBits = 9;
    constexpr int ExponentBias = 15;

    const auto r_mantissa = static_cast<float>(packed & 0x1ffu);
    const auto g_mantissa = static_cast<float>((packed >> 9) & 0x1ffu);
    const auto b_mantissa = static_cast<float>((packed >> 18) & 0x1ffu);
    const auto exponent = static_cast<int>((packed >> 27) & 0x1fu);

    const float scale = std::exp2(static_cast<float>(exponent - ExponentBias - MantissaBits));
    return {r_mantissa * scale, g_mantissa * scale, b_mantissa * scale, 1.f};
}

uint32_t Image2D::PackRGB9E5(const Vector3 &linear)
{
    constexpr int MantissaBits = 9;
    constexpr int ExponentBias = 15;
    constexpr int MaxExponent = 31;
    constexpr float MaxValue = 511.f / 512.f * 65536.f;

    const float r = std::clamp(linear.x(), 0.f, MaxValue);
    const float g = std::clamp(linear.y(), 0.f, MaxValue);
    const float b = std::clamp(linear.z(), 0.f, MaxValue);
    const float max_channel = std::max({r, g, b});

    if (max_channel < std::exp2(static_cast<float>(-ExponentBias - MantissaBits)))
    {
        return 0;
    }

    int exponent = std::max(-ExponentBias - 1, static_cast<int>(std::floor(std::log2(max_channel)))) + 1 + ExponentBias;
    float denom = std::exp2(static_cast<float>(exponent - ExponentBias - MantissaBits));
    if (static_cast<int>(std::floor(max_channel / denom + 0.5f)) == (1 << MantissaBits))
    {
        denom *= 2.f;
        exponent += 1;
    }
    exponent = std::clamp(exponent, 0, MaxExponent);

    const auto to_mantissa = [denom](float value) {
        return std::min(511u, static_cast<uint32_t>(std::floor(value / denom + 0.5f)));
    };

    return (static_cast<uint32_t>(exponent) << 27) | (to_mantissa(b) << 18) | (to_mantissa(g) << 9) | to_mantissa(r);
}

Image2D Image2D::CreateFromRawPixels(const uint8_t *data, unsigned width, unsigned height, PixelFormat source_format)
{
    Image2D image(width, height, PixelFormat::R8G8B8A8Srgb);
    ConvertPixelsToRGBA8(data, image.pixels_.data(), width * height, source_format);
    return image;
}

static void WriteImageData(void *context, void *data, int size)
{
    auto &buffer = *reinterpret_cast<std::vector<char> *>(context);

    auto old_size = buffer.size();
    buffer.resize(old_size + static_cast<unsigned>(size));

    std::memcpy(buffer.data() + old_size, data, static_cast<unsigned>(size));
}

bool Image2D::LoadFromFile(const std::string &file_path)
{
    std::filesystem::path fs_file_path(file_path);

    auto *file_manager = FileManager::GetNativeFileManager();

    // try two locations: packed folder and generated folder
    auto data = file_manager->Read(Path::Resource(file_path));
    if (data.empty())
    {
        data = file_manager->Read(Path::Internal(file_path));
        if (data.empty())
        {
            Log(Error, "failed to read image file {}", file_path);
            return false;
        }
    }

    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        Log(Error, "image file is too large to decode: {}", file_path);
        return false;
    }

    const auto *encoded_data = reinterpret_cast<const unsigned char *>(data.data());
    const auto encoded_size = static_cast<int>(data.size());
    constexpr int OutputChannelCount = 4;

    int decoded_width = 0;
    int decoded_height = 0;
    int source_channel_count = 0;
    Image2D decoded_image;

    if (stbi_is_hdr_from_memory(encoded_data, encoded_size))
    {
        auto loaded_pixels = OwnStbiPixels(stbi_loadf_from_memory(
            encoded_data, encoded_size, &decoded_width, &decoded_height, &source_channel_count, OutputChannelCount));
        if (!loaded_pixels)
        {
            const char *reason = stbi_failure_reason();
            Log(Error, "failed to decode image {}: {}", file_path, reason ? reason : "unknown error");
            return false;
        }

        decoded_image = Image2D(static_cast<unsigned>(decoded_width), static_cast<unsigned>(decoded_height),
                                PixelFormat::RGBAFloat16);
        const float half_max = static_cast<float>(std::numeric_limits<Half>::max());
        TaskManager::ParallelFor(0u, decoded_image.height_,
                                 [&decoded_image, source = loaded_pixels.get(), half_max](unsigned j) {
                                     auto *row = decoded_image.AccessRow<Half>(j);
                                     for (auto i = 0u; i < decoded_image.width_; i++)
                                     {
                                         for (auto channel = 0u; channel < OutputChannelCount; channel++)
                                         {
                                             const auto value =
                                                 source[(j * decoded_image.width_ + i) * OutputChannelCount + channel];
                                             row[i * OutputChannelCount + channel] =
                                                 static_cast<Half>(std::clamp(value, -half_max, half_max));
                                         }
                                     }
                                 })
            .wait();
    }
    else
    {
        auto loaded_pixels = OwnStbiPixels(stbi_load_from_memory(
            encoded_data, encoded_size, &decoded_width, &decoded_height, &source_channel_count, OutputChannelCount));
        if (!loaded_pixels)
        {
            const char *reason = stbi_failure_reason();
            Log(Error, "failed to decode image {}: {}", file_path, reason ? reason : "unknown error");
            return false;
        }

        decoded_image = Image2D(static_cast<unsigned>(decoded_width), static_cast<unsigned>(decoded_height),
                                PixelFormat::R8G8B8A8Unorm);
        std::memcpy(decoded_image.pixels_.data(), loaded_pixels.get(), decoded_image.GetStorageSize());
    }

    decoded_image.name_ = fs_file_path.filename().string();
    *this = std::move(decoded_image);

    return true;
}

const Image2D &Image2D::EnsureDecoded() const
{
    ASSERT(IsCompressedFormat(pixel_format_) && decode_cache_);
    std::call_once(decode_cache_->once,
                   [this] { decode_cache_->image = std::make_shared<Image2D>(TextureCompression::Decode(*this)); });
    return *decode_cache_->image;
}

uint32_t Image2D::GetContentHash() const
{
    CRC32 hasher;
    hasher.add(pixels_.data(), pixels_.size());
    return FinishContentHash(hasher, width_, height_, pixel_format_);
}

bool Image2D::WriteToFile(const Path &file_path) const
{
    if (IsCompressedFormat(pixel_format_))
    {
        return EnsureDecoded().WriteToFile(file_path);
    }

    std::vector<char> buffer;
    auto *custom_data = static_cast<void *>(&buffer);

    int encode_success = 0;
    if (IsHDRFormat(pixel_format_))
    {
        if (pixel_format_ == PixelFormat::RGBAFloat)
        {
            encode_success = stbi_write_hdr_to_func(&WriteImageData, custom_data, static_cast<int>(width_),
                                                    static_cast<int>(height_), static_cast<int>(channel_count_),
                                                    reinterpret_cast<const float *>(pixels_.data()));
        }
        else
        {
            Image2D full_precision_image(width_, height_, PixelFormat::RGBAFloat);
            if (full_precision_image.CopyFrom(*this))
            {
                encode_success =
                    stbi_write_hdr_to_func(&WriteImageData, custom_data, static_cast<int>(width_),
                                           static_cast<int>(height_), static_cast<int>(channel_count_),
                                           reinterpret_cast<const float *>(full_precision_image.pixels_.data()));
            }
        }
    }
    else
    {
        encode_success = stbi_write_png_to_func(&WriteImageData, custom_data, static_cast<int>(width_),
                                                static_cast<int>(height_), static_cast<int>(channel_count_),
                                                pixels_.data(), static_cast<int>(GetPixelSize(pixel_format_) * width_));
    }

    if (encode_success == 0)
    {
        Log(Error, "Failed to encode image {}", file_path.path.string());
        return false;
    }

    auto saved_path = FileManager::GetNativeFileManager()->Write(file_path, buffer);
    if (saved_path.empty())
    {
        Log(Error, "Failed to save image {}", file_path.path.string());
        return false;
    }

    Log(Info, "Image saved to {}", saved_path);
    return true;
}

bool Image2D::CopyFrom(const Image2D &other)
{
    if (width_ != other.width_ || height_ != other.height_)
    {
        Log(Error, "failed to copy image. source size {} * {}. destination size {} * {}", other.width_, other.height_,
            width_, height_);

        return false;
    }

    TaskManager::ParallelFor(0u, height_, [this, &other](unsigned j) {
        for (auto i = 0u; i < width_; i++)
        {
            this->SetPixel(i, j, other.AccessPixel(i, j));
        }
    }).wait();

    name_ = other.name_;

    return true;
}

void Image2DCube::DirectionToTextureCoordinate(const Vector3 &direction, Vector2 &out_uv, FaceId &out_face_id)
{
    float x = direction.x();
    float y = direction.y();
    float z = direction.z();
    float abs_x = std::abs(x);
    float abs_y = std::abs(y);
    float abs_z = std::abs(z);

    if (abs_x >= abs_y && abs_x >= abs_z)
    {
        if (x > 0)
        {
            out_face_id = FaceId::PositiveX;
            out_uv = Vector2(-z / x, -y / x);
        }
        else
        {
            out_face_id = FaceId::NegativeX;
            out_uv = Vector2(z / -x, -y / -x);
        }
    }
    else if (abs_y >= abs_x && abs_y >= abs_z)
    {
        if (y > 0)
        {
            out_face_id = FaceId::PositiveY;
            out_uv = Vector2(x / y, z / y);
        }
        else
        {
            out_face_id = FaceId::NegativeY;
            out_uv = Vector2(x / -y, -z / -y);
        }
    }
    else
    {
        if (z > 0)
        {
            out_face_id = FaceId::PositiveZ;
            out_uv = Vector2(x / z, -y / z);
        }
        else
        {
            out_face_id = FaceId::NegativeZ;
            out_uv = Vector2(-x / -z, -y / -z);
        }
    }

    out_uv = (out_uv + Vector2::Ones()) * 0.5f;
}

Vector3 Image2DCube::Sample(const Vector3 &direction) const
{
    Vector2 uv;
    FaceId face_id;

    DirectionToTextureCoordinate(direction, uv, face_id);
    return GetFace(face_id).Sample(uv);
}

uint32_t Image2DCube::GetContentHash() const
{
    if (content_hash_valid_.load(std::memory_order_acquire))
    {
        return content_hash_.load(std::memory_order_relaxed);
    }

    // concurrent first calls redundantly compute the same value, which is benign
    CRC32 hasher;
    for (const auto &face : faces_)
    {
        hasher.add(face->GetRawData(), face->GetStorageSize());
    }
    const uint32_t hash = FinishContentHash(hasher, GetWidth(), GetHeight(), GetFormat());

    content_hash_.store(hash, std::memory_order_relaxed);
    content_hash_valid_.store(true, std::memory_order_release);
    return hash;
}
} // namespace sparkle
