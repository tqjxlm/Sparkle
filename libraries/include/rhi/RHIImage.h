#pragma once

#include "rhi/RHIResource.h"

#include "core/Hash.h"
#include "io/ImageTypes.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIImageView.h"
#include "rhi/RHIMemory.h"

namespace sparkle
{
enum class RHIImageLayout : uint8_t
{
    Undefined,
    General,
    Read,
    StorageWrite,
    ColorOutput,
    DepthStencilOutput,
    TransferSrc,
    TransferDst,
    PreInitialized,
    Present,
};

enum class RHIPipelineStage : uint8_t
{
    Top,
    DrawIndirect,
    VertexInput,
    VertexShader,
    PixelShader,
    EarlyZ,
    LateZ,
    ColorOutput,
    ComputeShader,
    Transfer,
    Bottom,
};

class RHISampler : public RHIResource
{
public:
    enum class SamplerAddressMode : uint8_t
    {
        Repeat,
        RepeatMirror,
        ClampToEdge,
        ClampToBorder,
        Count
    };

    enum class BorderColor : uint8_t
    {
        IntTransparentBlack,
        FloatTransparentBlack,
        IntOpaqueBlack,
        FloatOpaqueBlack,
        IntOpaqueWhite,
        FloatOpaqueWhite,
        Count
    };

    enum class FilteringMethod : uint8_t
    {
        Nearest,
        Linear,
        Count
    };

    struct SamplerAttribute
    {
        SamplerAddressMode address_mode = SamplerAddressMode::Count;
        BorderColor border_color = BorderColor::Count;
        FilteringMethod filtering_method_min = FilteringMethod::Count;
        FilteringMethod filtering_method_mag = FilteringMethod::Count;
        FilteringMethod filtering_method_mipmap = FilteringMethod::Count;
        uint8_t min_lod = 0;
        uint8_t max_lod = 0;
        bool enable_anisotropy = true;

        auto operator<=>(const SamplerAttribute &) const = default;

        [[nodiscard]] uint32_t GetHash() const
        {
            uint32_t hash = 0;
            HashCombine(hash, address_mode);
            HashCombine(hash, border_color);
            HashCombine(hash, filtering_method_min);
            HashCombine(hash, filtering_method_mag);
            HashCombine(hash, filtering_method_mipmap);
            HashCombine(hash, min_lod);
            HashCombine(hash, max_lod);
            HashCombine(hash, enable_anisotropy);
            return hash;
        }
    };

    explicit RHISampler(const SamplerAttribute &attribute, const std::string &name)
        : RHIResource(name), attribute_(attribute)
    {
    }

protected:
    SamplerAttribute attribute_;
};

class RHIImage : public RHIResource
{
public:
    enum class ImageUsage : uint8_t
    {
        Undefined = 0,
        TransferDst = 1u << 0,
        TransferSrc = 1u << 1,
        Texture = 1u << 2,
        SRV = 1u << 3,
        UAV = 1u << 4,
        ColorAttachment = 1u << 5,
        DepthStencilAttachment = 1u << 6,
        TransientAttachment = 1u << 7,
    };

    enum class ImageType : uint8_t
    {
        Image2D,
        Image2DCube,
    };

    struct Attribute
    {
        PixelFormat format = PixelFormat::Count;
        RHISampler::SamplerAttribute sampler;
        uint32_t width = 1;
        uint32_t height = 1;
        RHIImage::ImageUsage usages = RHIImage::ImageUsage::Undefined;
        RHIMemoryProperty memory_properties = RHIMemoryProperty::None;
        uint8_t mip_levels = 1;
        uint8_t msaa_samples = 1;
        RHIImageLayout initial_layout = RHIImageLayout::Undefined;
        ImageType type = ImageType::Image2D;

        // this hash does not consider all attributes. it only ensures shader compatilibity.
        // i.e. if two images share the same attriute hash, they can be used in the same shader.
        [[nodiscard]] uint32_t GetHashForShader() const
        {
            uint32_t hash = 0;
            HashCombine(hash, format);
            HashCombine(hash, sampler.GetHash());
            HashCombine(hash, usages);
            HashCombine(hash, memory_properties);
            HashCombine(hash, type);
            return hash;
        }
    };

    struct TransitionRequest
    {
        RHIImageLayout target_layout;
        RHIPipelineStage after_stage;
        RHIPipelineStage before_stage;
        // by default, all mips are transitioned
        unsigned base_mip = 0;
        // by default, all mips are transitioned
        unsigned mip_count = 0;
    };

    RHIImage(const Attribute &attributes, const std::string &name);

    [[nodiscard]] RHIResourceRef<RHISampler> GetSampler() const
    {
        return sampler_;
    }

    [[nodiscard]] RHIResourceRef<RHIImageView> GetView(RHIContext *rhi, const RHIImageView::Attribute &attribute);

    [[nodiscard]] RHIResourceRef<RHIImageView> GetDefaultView(RHIContext *rhi);

#pragma region IO

    std::string SaveToFile(const std::string &file_path, RHIContext *rhi);

    bool LoadFromFile(const std::string &file_path);

#pragma endregion

#pragma region RHIImage Interface

    virtual void Transition(const TransitionRequest &request) = 0;

    virtual void Upload(const uint8_t *data) = 0;

    virtual void UploadFaces(std::array<const uint8_t *, 6> data) = 0;

    virtual void CopyToBuffer(const RHIBuffer *buffer) const = 0;

    virtual void CopyToImage(const RHIImage *image) const = 0;

    virtual void BlitToImage(const RHIImage *image, RHISampler::FilteringMethod filter) const = 0;

    virtual void GenerateMips() = 0;

#pragma endregion

#pragma region Attributes

    [[nodiscard]] Attribute GetAttributes() const
    {
        return attributes_;
    }

    [[nodiscard]] uint32_t GetHeight(uint32_t mip_level = 0) const
    {
        return attributes_.height >> mip_level;
    }

    [[nodiscard]] uint32_t GetWidth(uint32_t mip_level = 0) const
    {
        return attributes_.width >> mip_level;
    }

    [[nodiscard]] uint32_t GetBytesPerRow(uint32_t mip_level = 0) const
    {
        return GetPixelSize(attributes_.format) * GetWidth(mip_level);
    }

    [[nodiscard]] uint32_t GetStorageSize(uint32_t mip_level) const
    {
        return GetBytesPerRow(mip_level) * GetHeight(mip_level);
    }

    [[nodiscard]] uint32_t GetStorageSizePerLayer() const
    {
        uint32_t total_size = 0;
        for (auto i = 0u; i < attributes_.mip_levels; i++)
        {
            total_size += GetStorageSize(i);
        }
        return total_size;
    }

    [[nodiscard]] uint32_t GetStorageSize() const
    {
        switch (attributes_.type)
        {
        case ImageType::Image2D:
            return GetStorageSizePerLayer();
        case ImageType::Image2DCube:
            return GetStorageSizePerLayer() * 6;
        default:
            UnImplemented(attributes_.type);
            return 0;
        }
    }

#pragma endregion

#pragma region Bindless

    void SetBindlessId(uint32_t id)
    {
        bindless_id_ = id;
    }

    [[nodiscard]] bool IsRegisteredAsBindless() const
    {
        return bindless_id_ != UINT_MAX;
    }

    [[nodiscard]] uint32_t GetBindlessId() const
    {
        ASSERT(IsRegisteredAsBindless());
        return bindless_id_;
    }

#pragma endregion

#pragma region Layout

    [[nodiscard]] RHIImageLayout GetCurrentLayout(unsigned mip_level) const
    {
        return current_layout_[mip_level];
    }

    // CAUTION: normally this should not be used. use RHIImage::Transition instead unless you know what you are doing.
    void SetCurrentLayout(RHIImageLayout layout, unsigned base_mip, unsigned mip_count)
    {
        for (auto i = 0u; i < mip_count; i++)
        {
            current_layout_[base_mip + i] = layout;
        }
    }

#pragma endregion

protected:
    Attribute attributes_;

    RHIResourceRef<RHISampler> sampler_;

private:
    std::array<RHIImageLayout, 16> current_layout_;
    uint32_t bindless_id_ = UINT32_MAX;
    std::unordered_map<RHIImageView::Attribute, RHIResourceRef<RHIImageView>> image_views_;
};

RegisterEnumAsFlag(RHIImage::ImageUsage);
} // namespace sparkle

namespace std
{
template <> struct hash<sparkle::RHISampler::SamplerAttribute>
{
    size_t operator()(const sparkle::RHISampler::SamplerAttribute &k) const
    {
        // http://stackoverflow.com/a/1646913/126995
        size_t res = 17;

        res = res * 31 + hash<uint8_t>()(static_cast<uint8_t>(k.border_color));
        res = res * 31 + hash<uint8_t>()(static_cast<uint8_t>(k.address_mode));
        return res;
    }
};
} // namespace std
