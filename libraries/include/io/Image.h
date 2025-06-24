#pragma once

#include "core/math/Utilities.h"
#include "io/ImageTypes.h"

#include <vector>

namespace sparkle
{
class Image2D
{
public:
    Image2D() = default;

    Image2D(unsigned width, unsigned height, PixelFormat format)
        : pixel_format_(format), width_(width), height_(height), size_vector_{(width_ - 1), (height_ - 1)}
    {
        channel_count_ = GetFormatChannelCount(pixel_format_);
        pixels_.resize(width * height * GetPixelSize(format));
    }

    Image2D(unsigned width, unsigned height, PixelFormat format, const std::vector<uint8_t> &pixels)
        : Image2D(width, height, format)
    {
        pixels_.resize(width * height * GetPixelSize(format));
        memcpy(pixels_.data(), pixels.data(), pixels.size());
    }

    bool LoadFromFile(const std::string &file_path);

    bool CopyFrom(const Image2D &other);

    [[nodiscard]] std::string GetName() const
    {
        return name_;
    }

    [[nodiscard]] unsigned GetWidth() const
    {
        return width_;
    }

    [[nodiscard]] unsigned GetHeight() const
    {
        return height_;
    }

    [[nodiscard]] const uint8_t *GetRawData() const
    {
        return pixels_.data();
    }

    [[nodiscard]] size_t GetStorageSize() const
    {
        return pixels_.size();
    }

    [[nodiscard]] PixelFormat GetFormat() const
    {
        return pixel_format_;
    }

    [[nodiscard]] unsigned GetRowByteSize() const
    {
        return GetPixelSize(pixel_format_) * width_;
    }

    [[nodiscard]] bool IsValid() const
    {
        return !pixels_.empty() && pixels_.size() == (width_ * height_ * GetPixelSize(pixel_format_)) &&
               pixel_format_ != PixelFormat::Count;
    }

    [[nodiscard]] bool WriteToFile(const std::string &file_path) const;

    [[nodiscard]] Vector3 Sample(const Vector2 &uv) const
    {
        // a simple bilinear interpolation with uv wrapping
        using utilities::Lerp;
        using utilities::WrapMod;

        const Vector2 pixel_position = uv.cwiseProduct(size_vector_);
        int u_pixel;
        int v_pixel;
        float u_cell;
        float v_cell;
        utilities::Decompose(pixel_position.x(), u_pixel, u_cell);
        utilities::Decompose(pixel_position.y(), v_pixel, v_cell);

        const auto &sample_00 = AccessPixel(WrapMod(u_pixel, width_), WrapMod(v_pixel, height_));
        const auto &sample_10 = AccessPixel(WrapMod(u_pixel + 1, width_), WrapMod(v_pixel, height_));
        const auto &sample_01 = AccessPixel(WrapMod(u_pixel, width_), WrapMod(v_pixel + 1, height_));
        const auto &sample_11 = AccessPixel(WrapMod(u_pixel + 1, width_), WrapMod(v_pixel + 1, height_));

        // lerp u direction
        const auto &lerp_0 = Lerp(sample_00, sample_10, u_cell);
        const auto &lerp_1 = Lerp(sample_01, sample_11, u_cell);

        // lerp v direction
        auto sampled = Lerp(lerp_0, lerp_1, v_cell);

        Vector3 sampled_rgb = sampled.head<3>();

        // pseudo srgb decode
        if (IsSRGBFormat(pixel_format_))
        {
            return utilities::SRGBtoLinear(sampled_rgb);
        }

        return sampled_rgb;
    }

    void SetPixel(unsigned x, unsigned y, const Vector3 &value)
    {
        const bool convert_srgb = IsSRGBFormat(pixel_format_);
        const bool swizzle_b_g = IsSwizzeldFormat(pixel_format_);

        const auto &color = utilities::ConcatVector(convert_srgb ? utilities::LinearToSrgb(value) : value, 1.f);

        switch (pixel_format_)
        {
        case PixelFormat::R8G8B8A8_SRGB:
        case PixelFormat::R8G8B8A8_UNORM:
        case PixelFormat::B8G8R8A8_SRGB:
        case PixelFormat::B8G8R8A8_UNORM: {
            auto color_int = utilities::VecToColor(color);
            SetPixel<Color4>(x, y, swizzle_b_g ? utilities::Rgba2Bgra(color_int) : color_int);
            break;
        }
        case PixelFormat::RGBAFloat: {
            SetPixel<Vector4>(x, y, swizzle_b_g ? utilities::Rgba2Bgra(color) : color);
            break;
        }
        case PixelFormat::RGBAFloat16: {
            Vector4h color_half = (swizzle_b_g ? utilities::Rgba2Bgra(color) : color).cast<Half>();
            SetPixel<Vector4h>(x, y, color_half);
            break;
        }
        default:
            UnImplemented(pixel_format_);
            break;
        }
    }

    [[nodiscard]] Vector4 AccessPixel(unsigned x, unsigned y) const
    {
        switch (pixel_format_)
        {
        case PixelFormat::R8G8B8A8_SRGB:
        case PixelFormat::R8G8B8A8_UNORM:
        case PixelFormat::B8G8R8A8_SRGB:
        case PixelFormat::B8G8R8A8_UNORM:
            return utilities::ColorToVec(AccessPixel<Color4>(x, y));
        case PixelFormat::RGBAFloat:
            return AccessPixel<Vector4>(x, y);
        case PixelFormat::RGBAFloat16:
            return AccessPixel<Vector4h>(x, y).cast<float>();
        default:
            UnImplemented(pixel_format_);
        }

        return Vector4::Zero();
    }

private:
    template <typename T> void SetPixel(unsigned x, unsigned y, const T &value)
    {
        AccessPixel<T>(x, y) = value;
    }

    template <typename T> [[nodiscard]] const T &AccessPixel(unsigned x, unsigned y) const
    {
        return AccessRow<T>(y)[x];
    }

    template <typename T> T &AccessPixel(unsigned x, unsigned y)
    {
        return AccessRow<T>(y)[x];
    }

    template <typename T> [[nodiscard]] const T *AccessRow(unsigned y) const
    {
        return reinterpret_cast<const T *>(pixels_.data() + y * width_ * GetPixelSize(pixel_format_));
    }

    template <typename T> T *AccessRow(unsigned y)
    {
        return reinterpret_cast<T *>(pixels_.data() + y * width_ * GetPixelSize(pixel_format_));
    }

    PixelFormat pixel_format_ = PixelFormat::Count;
    unsigned width_;
    unsigned height_;
    unsigned channel_count_;

    Vector2 size_vector_;

    std::vector<uint8_t> pixels_;

    std::string name_ = "Image2D";
};

class Image2DCube
{
public:
    enum FaceId : uint8_t
    {
        PositiveX = 0,
        NegativeX = 1,
        PositiveY = 2,
        NegativeY = 3,
        PositiveZ = 4,
        NegativeZ = 5,
        Count = 6
    };

    static Vector3 TextureCoordinateToDirection(FaceId face_id, Scalar u, Scalar v)
    {
        Vector3 direction;
        switch (face_id)
        {
        case Image2DCube::FaceId::PositiveX:
            direction = Vector3(1.f, -v, -u);
            break;
        case Image2DCube::FaceId::NegativeX:
            direction = Vector3(-1.f, -v, u);
            break;
        case Image2DCube::FaceId::PositiveY:
            direction = Vector3(u, 1.f, v);
            break;
        case Image2DCube::FaceId::NegativeY:
            direction = Vector3(u, -1.f, -v);
            break;
        case Image2DCube::FaceId::PositiveZ:
            direction = Vector3(u, -v, 1.f);
            break;
        case Image2DCube::FaceId::NegativeZ:
            direction = Vector3(-u, -v, -1.f);
            break;
        default:
            ASSERT(false);
        }

        return direction.normalized();
    }

    static void DirectionToTextureCoordinate(const Vector3 &direction, Vector2 &out_uv, FaceId &out_face_id);

    Image2DCube() = default;

    Image2DCube(unsigned width, unsigned height, PixelFormat format, std::string name) : name_(std::move(name))
    {
        for (auto &face : faces_)
        {
            face = std::make_unique<Image2D>(width, height, format);
        }
    }

    [[nodiscard]] Image2D &GetFace(FaceId face_id) const
    {
        return *faces_[face_id];
    }

    [[nodiscard]] std::string GetName() const
    {
        return name_;
    }

    [[nodiscard]] unsigned GetWidth() const
    {
        return faces_[0]->GetWidth();
    }

    [[nodiscard]] unsigned GetHeight() const
    {
        return faces_[0]->GetHeight();
    }

    [[nodiscard]] PixelFormat GetFormat() const
    {
        return faces_[0]->GetFormat();
    }

    [[nodiscard]] Vector3 Sample(const Vector3 &direction) const;

private:
    std::array<std::unique_ptr<Image2D>, 6> faces_;

    std::string name_ = "Image2DCube";
};
} // namespace sparkle
