#pragma once

#if ENABLE_VULKAN

#include "VulkanMemory.h"

namespace sparkle
{
inline VkAccessFlags GetImageAccessFlags(const RHIImage *image, RHIImageLayout layout, RHIPipelineStage stage)
{
    auto usages = image->GetAttributes().usages;

    if (stage == RHIPipelineStage::Top || stage == RHIPipelineStage::Bottom)
    {
        return 0;
    }

    if (layout == RHIImageLayout::Undefined || layout == RHIImageLayout::Present)
    {
        return 0;
    }

    if (layout == RHIImageLayout::TransferDst)
    {
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    if (layout == RHIImageLayout::TransferSrc)
    {
        return VK_ACCESS_TRANSFER_READ_BIT;
    }

    if (stage == RHIPipelineStage::DrawIndirect)
    {
        return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    }

    if (layout == RHIImageLayout::StorageWrite)
    {
        return VK_ACCESS_SHADER_WRITE_BIT;
    }

    if (layout == RHIImageLayout::ColorOutput)
    {
        ASSERT(usages & RHIImage::ImageUsage::ColorAttachment);
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (layout == RHIImageLayout::DepthStencilOutput)
    {
        ASSERT(usages & RHIImage::ImageUsage::DepthStencilAttachment);
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    if (layout == RHIImageLayout::Read)
    {
        // TODO(tqjxlm): handle subpass read
        // if (usages & RHIImage::ImageUsage::DepthStencilAttachment)
        // {
        //     return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        // }
        // if (usages & RHIImage::ImageUsage::ColorAttachment)
        // {
        //     return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        // }
        return VK_ACCESS_SHADER_READ_BIT;
    }

    // if it makes it here, some unexpected behaviour happens.
    // Check whether api usage is correct or whether this function should be extended
    ASSERT_F(false, "unexpected transition status. layout {}. stage {}", static_cast<int>(layout),
             static_cast<int>(stage));

    return 0;
};

inline VkFilter GetVulkanFilteringMethod(RHISampler::FilteringMethod filtering_method)
{
    switch (filtering_method)
    {
    case RHISampler::FilteringMethod::Nearest:
        return VK_FILTER_NEAREST;
    case RHISampler::FilteringMethod::Linear:
        return VK_FILTER_LINEAR;
    default:
        UnImplemented(filtering_method);
        return VK_FILTER_MAX_ENUM;
    }
}

inline VkBorderColor GetVulkanBorderColor(RHISampler::BorderColor color)
{
    switch (color)
    {
    case RHISampler::BorderColor::IntTransparentBlack:
        return VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    case RHISampler::BorderColor::FloatTransparentBlack:
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case RHISampler::BorderColor::IntOpaqueBlack:
        return VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    case RHISampler::BorderColor::FloatOpaqueBlack:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case RHISampler::BorderColor::IntOpaqueWhite:
        return VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    case RHISampler::BorderColor::FloatOpaqueWhite:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    default:
        UnImplemented(color);
        return VK_BORDER_COLOR_MAX_ENUM;
    }
}

inline VkSamplerMipmapMode GetVulkanMipmapMethod(RHISampler::FilteringMethod filtering_method)
{
    switch (filtering_method)
    {
    case RHISampler::FilteringMethod::Nearest:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    case RHISampler::FilteringMethod::Linear:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    default:
        UnImplemented(filtering_method);
        return VK_SAMPLER_MIPMAP_MODE_MAX_ENUM;
    }
}

inline VkPipelineStageFlags GetVulkanPipelineStage(RHIPipelineStage stage)
{
    switch (stage)
    {
    case RHIPipelineStage::Top:
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    case RHIPipelineStage::DrawIndirect:
        return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    case RHIPipelineStage::VertexInput:
        return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    case RHIPipelineStage::VertexShader:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    case RHIPipelineStage::PixelShader:
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case RHIPipelineStage::EarlyZ:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    case RHIPipelineStage::LateZ:
        return VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case RHIPipelineStage::ColorOutput:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case RHIPipelineStage::ComputeShader:
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case RHIPipelineStage::Transfer:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case RHIPipelineStage::Bottom:
        return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    UnImplemented(stage);
    return VK_PIPELINE_STAGE_NONE_KHR;
};

inline VkSampleCountFlagBits GetVkMsaaSampleBit(uint32_t sample_count)
{
    switch (sample_count)
    {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    case 32:
        return VK_SAMPLE_COUNT_32_BIT;
    case 64:
        return VK_SAMPLE_COUNT_64_BIT;
    default:
        ASSERT_F(false, "Unsupported msaa sample {}", sample_count);
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline VkFormat GetVkPixelFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case PixelFormat::B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case PixelFormat::R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case PixelFormat::R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::RGBAFloat:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case PixelFormat::RGBAFloat16:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::RGBAUInt32:
        return VK_FORMAT_R32G32B32A32_UINT;
    case PixelFormat::D24_S8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case PixelFormat::D32:
        return VK_FORMAT_D32_SFLOAT;
    case PixelFormat::R10G10B10A2_UNORM:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case PixelFormat::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case PixelFormat::R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    default:
        ASSERT_F(false, "Unsupported image format {}", static_cast<int>(format));
        return VK_FORMAT_UNDEFINED;
    }
}

inline VkImageUsageFlags GetVkImageUsage(RHIImage::ImageUsage usage)
{
    VkImageUsageFlags flags = 0;
    if (usage & RHIImage::ImageUsage::TransferDst)
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage & RHIImage::ImageUsage::TransferSrc)
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & RHIImage::ImageUsage::Texture)
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage & RHIImage::ImageUsage::UAV || usage & RHIImage::ImageUsage::SRV)
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (usage & RHIImage::ImageUsage::ColorAttachment)
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage & RHIImage::ImageUsage::DepthStencilAttachment)
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return flags;
}

inline VkImageLayout GetVulkanImageLayout(RHIImageLayout rhi_layout)
{
    switch (rhi_layout)
    {
    case RHIImageLayout::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case RHIImageLayout::General:
        return VK_IMAGE_LAYOUT_GENERAL;
    case RHIImageLayout::PreInitialized:
        return VK_IMAGE_LAYOUT_PREINITIALIZED;
    case RHIImageLayout::Read:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case RHIImageLayout::StorageWrite:
        return VK_IMAGE_LAYOUT_GENERAL;
    case RHIImageLayout::TransferSrc:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case RHIImageLayout::TransferDst:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case RHIImageLayout::Present:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case RHIImageLayout::ColorOutput:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case RHIImageLayout::DepthStencilOutput:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    return VK_IMAGE_LAYOUT_UNDEFINED;
};

inline PixelFormat VkFormatToPixelFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return PixelFormat::B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return PixelFormat::B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return PixelFormat::R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return PixelFormat::R8G8B8A8_UNORM;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return PixelFormat::RGBAFloat;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return PixelFormat::RGBAFloat16;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return PixelFormat::D24_S8;
    case VK_FORMAT_D32_SFLOAT:
        return PixelFormat::D32;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return PixelFormat::R10G10B10A2_UNORM;
    case VK_FORMAT_R32_UINT:
        return PixelFormat::R32_UINT;
    case VK_FORMAT_R32_SFLOAT:
        return PixelFormat::R32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT:
        return PixelFormat::RGBAUInt32;
    default:
        UnImplemented(format);
        return PixelFormat::Count;
    }
}

inline VkSamplerAddressMode GetVulkanSamplerAddressMode(RHISampler::SamplerAddressMode mode)
{
    switch (mode)
    {
    case RHISampler::SamplerAddressMode::Repeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case RHISampler::SamplerAddressMode::RepeatMirror:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case RHISampler::SamplerAddressMode::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case RHISampler::SamplerAddressMode::ClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
        UnImplemented(mode);
        return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
    }
}

class VulkanSampler : public RHISampler
{
public:
    VulkanSampler(RHISampler::SamplerAttribute attribute, const std::string &name);

    ~VulkanSampler() override;

    [[nodiscard]] VkSampler GetSampler() const
    {
        return sampler_;
    }

    void WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                         std::vector<VkWriteDescriptorSet> &out_set_write) const;

private:
    VkSampler sampler_;
};

class VulkanImageView : public RHIImageView
{
public:
    VulkanImageView(Attribute attribute, RHIImage *image);

    ~VulkanImageView() override;

    [[nodiscard]] VkImageView GetView() const
    {
        return image_view_;
    }

    void WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                         std::vector<VkWriteDescriptorSet> &out_set_write);

private:
    VkImageView image_view_;
};

class VulkanImage : public RHIImage
{
public:
    struct VulkanImageAttribute
    {
        explicit VulkanImageAttribute(const Attribute &attribute, VkFormat format_override)
        {
            if (format_override != VK_FORMAT_UNDEFINED)
            {
                format = format_override;
            }
            else
            {
                format = GetVkPixelFormat(attribute.format);
            }
            msaa_samples = GetVkMsaaSampleBit(attribute.msaa_samples);
            usages = GetVkImageUsage(attribute.usages);
            memory_properties = GetVulkanMemoryPropertyFlags(attribute.memory_properties);
        }

        VkFormat format;
        VkImageUsageFlags usages;
        VkMemoryPropertyFlags memory_properties;
        VkSampleCountFlagBits msaa_samples;
    };

    VulkanImage(const Attribute &attribute, VkFormat format_override, const std::string &name)
        : RHIImage(attribute, name), vulkan_attributes_(VulkanImageAttribute(attribute, format_override))
    {
        if (format_override != VK_FORMAT_UNDEFINED)
        {
            attributes_.format = VkFormatToPixelFormat(format_override);
        }

        ASSERT(attributes_.format != PixelFormat::Count);

        CreateImage();
        CreateSampler();
    }

    VulkanImage(const Attribute &attribute, VkFormat format_override, VkImage image, const std::string &name)
        : RHIImage(attribute, name), external_(true),
          vulkan_attributes_(VulkanImageAttribute(attribute, format_override)), image_(image)
    {
        attributes_.format = VkFormatToPixelFormat(format_override);

        CreateSampler();
    }

    ~VulkanImage() override;

    void Transition(const TransitionRequest &request) override;

    void Upload(const uint8_t *data) override;

    void UploadFaces(std::array<const uint8_t *, 6> data) override;

    void CopyToImage(const RHIImage *image) const override;

    void CopyToBuffer(const RHIBuffer *buffer) const override;

    void GenerateMips() override;

    void BlitToImage(const RHIImage *image, RHISampler::FilteringMethod filter) const override;

    void BlitToImage(const RHIImage *image, uint8_t from_mip, uint8_t to_mip,
                     RHISampler::FilteringMethod filtering) const;

    void TransitionLayout(VkCommandBuffer command_buffer, const TransitionRequest &request);

    [[nodiscard]] VkImage GetImage() const
    {
        return image_;
    }

    [[nodiscard]] VkImageLayout GetVkLayout(unsigned mip_level) const
    {
        return GetVulkanImageLayout(GetCurrentLayout(mip_level));
    }

    [[nodiscard]] VkImageAspectFlags GetAspect() const
    {
        switch (vulkan_attributes_.format)
        {
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    [[nodiscard]] const auto &GetVulkanAttributes() const
    {
        return vulkan_attributes_;
    }

private:
    void CreateImage();

    void CreateSampler();

    bool external_ = false;
    VulkanImageAttribute vulkan_attributes_;
    VkImage image_;
    VmaAllocation allocation_;
};
} // namespace sparkle

#endif
