#include "io/Image.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "io/ImageTypes.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#pragma clang diagnostic pop

#include <filesystem>

namespace sparkle
{
static void WriteImageFile(void *context, void *data, int size)
{
    const auto &path = *static_cast<std::string *>(context);
    auto *file_manager = FileManager::GetNativeFileManager();
    auto saved_path =
        file_manager->WriteFile(path, reinterpret_cast<const char *>(data), static_cast<uint64_t>(size), true);
    if (saved_path.empty())
    {
        Log(Error, "Failed to save image {}", path);
    }
    else
    {
        Log(Info, "Image saved to {}", saved_path);
    }
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
    bool is_hdr = fs_file_path.extension().string() == ".hdr";

    auto *file_manager = FileManager::GetNativeFileManager();

    // try two locations: packed folder and generated folder
    auto data = file_manager->ReadResource(file_path);
    if (data.empty())
    {
        data = file_manager->ReadFile(file_path, false);
        if (data.empty())
        {
            Log(Error, "failed to read image file {}", file_path);
            return false;
        }
    }

    const int force_channel_count = 4;

    if (is_hdr)
    {
        auto *loaded_pixels =
            stbi_loadf_from_memory(reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()),
                                   reinterpret_cast<int *>(&width_), reinterpret_cast<int *>(&height_),
                                   reinterpret_cast<int *>(&channel_count_), force_channel_count);
        if (!loaded_pixels)
        {
            return false;
        }

        // TODO(tqjxlm): handle channel count as is without auto padding
        channel_count_ = force_channel_count;

        // TODO(tqjxlm): detect pixel format
        pixel_format_ = PixelFormat::RGBAFloat16;

        pixels_.resize(width_ * height_ * GetPixelSize(pixel_format_));

        if (pixel_format_ == PixelFormat::RGBAFloat)
        {
            TaskManager::ParallelFor(0u, height_, [this, loaded_pixels](unsigned j) {
                auto *row_data = AccessRow<float>(j);
                std::memcpy(row_data, reinterpret_cast<uint8_t *>(loaded_pixels) + j * GetRowByteSize(),
                            GetRowByteSize());
            }).wait();
        }
        else if (pixel_format_ == PixelFormat::RGBAFloat16)
        {
            TaskManager::ParallelFor(0u, height_, [this, loaded_pixels](unsigned j) {
                Vector4 *row_data = reinterpret_cast<Vector4 *>(loaded_pixels) + j * width_;
                for (auto i = 0u; i < width_; i++)
                {
                    Vector4h pixel_data = row_data[i].cast<Half>();
                    pixel_data = pixel_data.array().min(std::numeric_limits<Half>::max());

                    SetPixel<Vector4h>(i, j, pixel_data);
                }
            }).wait();
        }
        else
        {
            UnImplemented(pixel_format_);
        }
    }
    else
    {
        auto *loaded_pixels =
            stbi_load_from_memory(reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()),
                                  reinterpret_cast<int *>(&width_), reinterpret_cast<int *>(&height_),
                                  reinterpret_cast<int *>(&channel_count_), force_channel_count);
        if (!loaded_pixels)
        {
            return false;
        }

        // TODO(tqjxlm): handle channel count as is without auto padding
        channel_count_ = force_channel_count;

        // TODO(tqjxlm): detect pixel format. we always assume linear color for now.
        pixel_format_ = PixelFormat::R8G8B8A8_UNORM;

        pixels_.resize(width_ * height_ * channel_count_ * sizeof(float));

        TaskManager::ParallelFor(0u, height_, [this, loaded_pixels](unsigned j) {
            auto *row_data = AccessRow<uint8_t>(j);
            std::memcpy(row_data, reinterpret_cast<uint8_t *>(loaded_pixels) + j * GetRowByteSize(), GetRowByteSize());
        }).wait();
    }

    size_vector_ = {(width_ - 1), (height_ - 1)};

    name_ = fs_file_path.filename().string();

    return true;
}

bool Image2D::WriteToFile(const std::string &file_path) const
{
    int save_success = 0;
    if (IsHDRFormat(pixel_format_))
    {
        // TODO(tqjxlm): streaming file saving
        std::vector<char> buffer;
        auto *custom_data = static_cast<void *>(&buffer);

        if (pixel_format_ == PixelFormat::RGBAFloat16)
        {
            Image2D full_precision_image(width_, height_, PixelFormat::RGBAFloat);
            if (full_precision_image.CopyFrom(*this))
            {
                save_success =
                    stbi_write_hdr_to_func(&WriteImageData, custom_data, static_cast<int>(width_),
                                           static_cast<int>(height_), static_cast<int>(channel_count_),
                                           reinterpret_cast<const float *>(full_precision_image.pixels_.data()));
            }
        }
        else
        {
            save_success = stbi_write_hdr_to_func(&WriteImageData, custom_data, static_cast<int>(width_),
                                                  static_cast<int>(height_), static_cast<int>(channel_count_),
                                                  reinterpret_cast<const float *>(pixels_.data()));
        }

        auto *file_manager = FileManager::GetNativeFileManager();
        file_manager->WriteFile(file_path, buffer, true);
    }
    else
    {
        // stbi API needs a non-const pointer, but we know we don't modify it any way
        auto *custom_data = const_cast<void *>(static_cast<const void *>(&file_path));
        save_success = stbi_write_png_to_func(&WriteImageFile, custom_data, static_cast<int>(width_),
                                              static_cast<int>(height_), static_cast<int>(channel_count_),
                                              pixels_.data(), static_cast<int>(GetPixelSize(pixel_format_) * width_));
    }

    if (save_success == 1)
    {
        Log(Info, "Output ok. Path {}", file_path);
    }
    else
    {
        Log(Info, "Output failed. Path {}", file_path);
    }

    return save_success != 0;
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
} // namespace sparkle
