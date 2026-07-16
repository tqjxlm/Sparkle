#if ENABLE_VULKAN

#include "VulkanImage.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSet.h"

namespace sparkle
{
void VulkanImage::CreateImage()
{
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = attributes_.width;
    image_info.extent.height = attributes_.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = attributes_.mip_levels;
    image_info.arrayLayers = 1;
    image_info.format = vulkan_attributes_.format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = GetVulkanImageLayout(attributes_.initial_layout);
    image_info.usage = vulkan_attributes_.usages;
    image_info.samples = vulkan_attributes_.msaa_samples;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (attributes_.type == RHIImage::ImageType::Image2DCube)
    {
        image_info.arrayLayers = 6;
        image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.requiredFlags = vulkan_attributes_.memory_properties;

    auto result =
        vmaCreateImage(context->GetMemoryAllocator(), &image_info, &allocation_info, &image_, &allocation_, nullptr);
    ASSERT_F(result == VK_SUCCESS, "failed to create image! {}", static_cast<int>(result));

    context->SetDebugInfo(reinterpret_cast<uint64_t>(image_), VK_OBJECT_TYPE_IMAGE, GetName().c_str());
}

void VulkanImage::CreateSampler()
{
    if (attributes_.sampler.address_mode != RHISampler::SamplerAddressMode::Count)
    {
        sampler_ = context->GetRHI()->GetSampler(attributes_.sampler);
    }
}

void VulkanImage::TransitionLayout(VkCommandBuffer command_buffer, const TransitionRequest &request)
{
    ASSERT(command_buffer);
    ASSERT(request.base_mip < attributes_.mip_levels);
    ASSERT(request.base_array_layer < GetArrayLayerCount());

    const auto mip_count = request.mip_count == 0 ? attributes_.mip_levels - request.base_mip : request.mip_count;
    const auto array_layer_count =
        request.array_layer_count == 0 ? GetArrayLayerCount() - request.base_array_layer : request.array_layer_count;

    ASSERT(request.base_mip + mip_count <= attributes_.mip_levels);
    ASSERT(request.base_array_layer + array_layer_count <= GetArrayLayerCount());

    const VkPipelineStageFlags source_stage = GetVulkanPipelineStage(request.after_stage);
    const VkPipelineStageFlags destination_stage = GetVulkanPipelineStage(request.before_stage);
    const VkImageLayout new_layout = GetVulkanImageLayout(request.target_layout);

    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(mip_count * array_layer_count);

    auto transition_mip_range = [this, new_layout, &request, &barriers](RHIImageLayout old_layout, unsigned first_mip,
                                                                        unsigned last_mip, unsigned array_layer) {
        const auto old_vk_layout = GetVulkanImageLayout(old_layout);
        if (new_layout == old_vk_layout)
        {
            return;
        }

        auto &barrier = barriers.emplace_back(VkImageMemoryBarrier{});
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = old_vk_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange.aspectMask = GetAspect();
        barrier.subresourceRange.baseMipLevel = first_mip;
        barrier.subresourceRange.levelCount = last_mip - first_mip + 1;
        barrier.subresourceRange.baseArrayLayer = array_layer;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = GetImageAccessFlags(this, old_layout, request.after_stage);
        barrier.dstAccessMask = GetImageAccessFlags(this, request.target_layout, request.before_stage);
    };

    const auto mip_end = request.base_mip + mip_count;
    const auto array_layer_end = request.base_array_layer + array_layer_count;
    for (auto array_layer = request.base_array_layer; array_layer < array_layer_end; array_layer++)
    {
        auto range_start = request.base_mip;
        auto old_layout = GetCurrentLayout(range_start, array_layer);

        for (auto mip = range_start + 1; mip < mip_end; mip++)
        {
            if (GetCurrentLayout(mip, array_layer) == old_layout)
            {
                continue;
            }

            transition_mip_range(old_layout, range_start, mip - 1, array_layer);
            range_start = mip;
            old_layout = GetCurrentLayout(mip, array_layer);
        }

        transition_mip_range(old_layout, range_start, mip_end - 1, array_layer);
    }

    if (!barriers.empty())
    {
        vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(barriers.size()), barriers.data());
    }

    SetCurrentLayout(request.target_layout, request.base_mip, mip_count, request.base_array_layer, array_layer_count);
}

void VulkanImage::Upload(const uint8_t *data)
{
    auto image_size = GetStorageSize();
    RHIBuffer::Attribute staging_attribute{.size = image_size,
                                           .usages = RHIBuffer::BufferUsage::TransferSrc,
                                           .mem_properties =
                                               RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                           .is_dynamic = false};

    auto staging_buffer = context->GetRHI()->CreateBuffer(staging_attribute, "TextureStagingBuffer");
    // immediate and blocking upload
    staging_buffer->UploadImmediate(data);

    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();

    TransitionLayout(command_buffer, {.target_layout = RHIImageLayout::TransferDst,
                                      .after_stage = RHIPipelineStage::Top,
                                      .before_stage = RHIPipelineStage::Transfer});
    staging_buffer->CopyToImage(this);
    TransitionLayout(command_buffer, {.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::Transfer,
                                      .before_stage = RHIPipelineStage::Bottom});
}

void VulkanImage::UploadFaces(std::array<const uint8_t *, 6> data)
{
    ASSERT(attributes_.type == RHIImage::ImageType::Image2DCube);

    auto image_size = GetStorageSize();
    RHIBuffer::Attribute staging_attribute{.size = image_size,
                                           .usages = RHIBuffer::BufferUsage::TransferSrc,
                                           .mem_properties =
                                               RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                           .is_dynamic = false};

    auto staging_buffer = context->GetRHI()->CreateBuffer(staging_attribute, "TextureStagingBuffer");

    // immediate and blocking upload
    void *buffer = staging_buffer->Lock();
    for (auto i = 0u; i < 6; i++)
    {
        memcpy(buffer, data[i], GetStorageSizePerLayer());
        buffer = reinterpret_cast<uint8_t *>(buffer) + GetStorageSizePerLayer();
    }
    staging_buffer->UnLock();

    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();

    TransitionLayout(command_buffer, {.target_layout = RHIImageLayout::TransferDst,
                                      .after_stage = RHIPipelineStage::Top,
                                      .before_stage = RHIPipelineStage::Transfer});
    staging_buffer->CopyToImage(this);
    TransitionLayout(command_buffer, {.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::Transfer,
                                      .before_stage = RHIPipelineStage::Bottom});
}

void VulkanImage::CopyToImage(const RHIImage *image) const
{
    const auto *dst = RHICast<VulkanImage>(image);

    std::vector<VkImageCopy> copy_regions(attributes_.mip_levels);

    ASSERT_EQUAL(attributes_.mip_levels, dst->GetAttributes().mip_levels);

    for (auto mip_level = 0u; mip_level < attributes_.mip_levels; mip_level++)
    {
        ASSERT_EQUAL(GetWidth(mip_level), dst->GetWidth(mip_level));
        ASSERT_EQUAL(GetHeight(mip_level), dst->GetHeight(mip_level));

        auto &copy_region = copy_regions[mip_level];

        copy_region = {};

        copy_region.srcSubresource.aspectMask = GetAspect();
        copy_region.srcSubresource.mipLevel = mip_level;
        copy_region.srcSubresource.baseArrayLayer = 0;
        copy_region.srcSubresource.layerCount = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

        copy_region.dstSubresource = copy_region.srcSubresource;

        copy_region.extent = {.width = GetWidth(mip_level), .height = GetHeight(mip_level), .depth = 1};
    }

    vkCmdCopyImage(context->GetCurrentCommandBuffer(), image_, GetVkLayout(0), dst->GetImage(), dst->GetVkLayout(0),
                   static_cast<uint32_t>(copy_regions.size()), copy_regions.data());
}

void VulkanImage::GenerateMips()
{
    for (uint8_t i = 0u; i < attributes_.mip_levels - 1; i++)
    {
        Transition({.target_layout = RHIImageLayout::TransferSrc,
                    .after_stage = RHIPipelineStage::Bottom,
                    .before_stage = RHIPipelineStage::Transfer,
                    .base_mip = i,
                    .mip_count = 1});
        Transition({.target_layout = RHIImageLayout::TransferDst,
                    .after_stage = RHIPipelineStage::Bottom,
                    .before_stage = RHIPipelineStage::Transfer,
                    .base_mip = i + 1u,
                    .mip_count = 1});

        BlitToImage(this, i, i + 1, RHISampler::FilteringMethod::Linear);
    }
}

void VulkanImage::BlitToImage(const RHIImage *image, RHISampler::FilteringMethod filter) const
{
    ASSERT(attributes_.mip_levels == image->GetAttributes().mip_levels);
    ASSERT_EQUAL(attributes_.type, image->GetAttributes().type);
    ASSERT_EQUAL(attributes_.width, image->GetAttributes().width);
    ASSERT_EQUAL(attributes_.height, image->GetAttributes().height);

    for (uint8_t i = 0u; i < attributes_.mip_levels; i++)
    {
        BlitToImage(image, i, i, filter);
    }
}

void VulkanImage::BlitToImage(const RHIImage *image, uint8_t from_mip, uint8_t to_mip,
                              RHISampler::FilteringMethod filtering) const
{
    const auto *dst = RHICast<VulkanImage>(image);

    ASSERT(from_mip < attributes_.mip_levels);
    ASSERT(to_mip < dst->GetAttributes().mip_levels);
    ASSERT_EQUAL(GetAspect(), dst->GetAspect());

    VkImageBlit blit{};

    blit.srcOffsets[0] = {.x = 0, .y = 0, .z = 0};
    blit.srcOffsets[1] = {
        .x = static_cast<int32_t>(GetWidth(from_mip)), .y = static_cast<int32_t>(GetHeight(from_mip)), .z = 1};
    blit.srcSubresource.aspectMask = GetAspect();
    blit.srcSubresource.mipLevel = from_mip;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    blit.dstOffsets[0] = {.x = 0, .y = 0, .z = 0};
    blit.dstOffsets[1] = {
        .x = static_cast<int32_t>(dst->GetWidth(to_mip)), .y = static_cast<int32_t>(dst->GetHeight(to_mip)), .z = 1};
    blit.dstSubresource.aspectMask = dst->GetAspect();
    blit.dstSubresource.mipLevel = to_mip;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    VkFilter filter = GetVulkanFilteringMethod(filtering);

    vkCmdBlitImage(context->GetCurrentCommandBuffer(), image_, GetVkLayout(from_mip), dst->GetImage(),
                   dst->GetVkLayout(to_mip), 1, &blit, filter);
}

void VulkanImage::CopyToBuffer(const RHIBuffer *rhi_buffer) const
{
    const auto *buffer = RHICast<VulkanBuffer>(rhi_buffer);

    // copy to dynamic buffer is not supported for now
    ASSERT(!rhi_buffer->IsDynamic());

    uint32_t copied_bytes = 0;
    std::vector<VkBufferImageCopy> copy_regions(attributes_.mip_levels);

    unsigned num_layers = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    for (auto mip_level = 0u; mip_level < attributes_.mip_levels; mip_level++)
    {
        auto &copy_region = copy_regions[mip_level];

        copy_region = {};

        copy_region.bufferOffset = (buffer->GetOffset(UINT_MAX) + copied_bytes);
        copy_region.bufferRowLength = 0;
        copy_region.bufferImageHeight = 0;

        copy_region.imageSubresource.aspectMask = GetAspect();
        copy_region.imageSubresource.mipLevel = mip_level;
        copy_region.imageSubresource.baseArrayLayer = 0;
        copy_region.imageSubresource.layerCount = num_layers;

        copy_region.imageOffset = {.x = 0, .y = 0, .z = 0};
        copy_region.imageExtent = {.width = GetWidth(mip_level), .height = GetHeight(mip_level), .depth = 1};

        copied_bytes += GetStorageSize(mip_level) * num_layers;
    }

    vkCmdCopyImageToBuffer(context->GetCurrentCommandBuffer(), image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer->GetResourceThisFrame(), static_cast<unsigned>(copy_regions.size()),
                           copy_regions.data());
}

void VulkanImage::Transition(const TransitionRequest &request)
{
    TransitionLayout(context->GetCurrentCommandBuffer(), request);
}

VulkanSampler::VulkanSampler(RHISampler::SamplerAttribute attribute, const std::string &name)
    : RHISampler(attribute, name)
{
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = GetVulkanFilteringMethod(attribute.filtering_method_mag);
    sampler_info.minFilter = GetVulkanFilteringMethod(attribute.filtering_method_min);
    sampler_info.addressModeU = GetVulkanSamplerAddressMode(attribute.address_mode);
    sampler_info.addressModeV = GetVulkanSamplerAddressMode(attribute.address_mode);
    sampler_info.addressModeW = GetVulkanSamplerAddressMode(attribute.address_mode);
    sampler_info.anisotropyEnable = attribute.enable_anisotropy ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->GetPhysicalDevice(), &properties);
    sampler_info.maxAnisotropy = attribute.enable_anisotropy ? properties.limits.maxSamplerAnisotropy : 1.0f;
    if (attribute.address_mode == RHISampler::SamplerAddressMode::ClampToBorder)
    {
        sampler_info.borderColor = GetVulkanBorderColor(attribute.border_color);
    }
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = GetVulkanMipmapMethod(attribute.filtering_method_mipmap);
    sampler_info.minLod = attribute.min_lod;
    sampler_info.maxLod = attribute.max_lod;

    CHECK_VK_ERROR(vkCreateSampler(context->GetDevice(), &sampler_info, nullptr, &sampler_));

    context->SetDebugInfo(reinterpret_cast<uint64_t>(sampler_), VK_OBJECT_TYPE_SAMPLER, GetName().c_str());
}

VulkanSampler::~VulkanSampler()
{
    vkDestroySampler(context->GetDevice(), sampler_, nullptr);
}

VulkanImage::~VulkanImage()
{
    for (auto &[view_attribute, view] : image_views_)
    {
        RHICast<VulkanImageView>(view.get())->DestroyHandle();
    }

    if (!external_)
    {
        // if we own this resource, destroy it
        vmaDestroyImage(context->GetMemoryAllocator(), image_, allocation_);
    }
}

void VulkanSampler::WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                                    std::vector<VkWriteDescriptorSet> &out_set_write) const
{
    auto &set_write = out_set_write.emplace_back(VkWriteDescriptorSet{});
    InitDescriptorWrite(set_write, slot, descriptor_set, 0, descriptor_type);

    auto &info = *context->GetRHI()->AllocateOneFrameMemory<VkDescriptorImageInfo>();

    info = {};
    info.sampler = GetSampler();

    set_write.pImageInfo = &info;
}

VulkanImageView::VulkanImageView(Attribute attribute, RHIImage *image) : RHIImageView(std::move(attribute), image)
{
    auto *vulkan_image = RHICast<VulkanImage>(image);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = vulkan_image->GetImage();
    view_info.format = vulkan_image->GetVulkanAttributes().format;
    view_info.subresourceRange.aspectMask = vulkan_image->GetAspect();
    view_info.subresourceRange.baseMipLevel = attribute_.base_mip_level;
    view_info.subresourceRange.levelCount = attribute_.mip_level_count;
    view_info.subresourceRange.baseArrayLayer = attribute_.base_array_layer;
    view_info.subresourceRange.layerCount = attribute_.array_layer_count;

    switch (attribute_.type)
    {
    case ImageViewType::Image2D:
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case ImageViewType::Image2DCube:
        view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    case ImageViewType::Image2DArray:
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    default:
        UnImplemented(attribute_.type);
        break;
    }

    CHECK_VK_ERROR(vkCreateImageView(context->GetDevice(), &view_info, nullptr, &image_view_));
}

VulkanImageView::~VulkanImageView()
{
    DestroyHandle();
}

void VulkanImageView::DestroyHandle()
{
    if (image_view_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(context->GetDevice(), image_view_, nullptr);
        image_view_ = VK_NULL_HANDLE;
    }
}

void VulkanImageView::WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                                      std::vector<VkWriteDescriptorSet> &out_set_write) const
{
    auto &set_write = out_set_write.emplace_back(VkWriteDescriptorSet{});
    InitDescriptorWrite(set_write, slot, descriptor_set, 0, descriptor_type);

    auto &info = *context->GetRHI()->AllocateOneFrameMemory<VkDescriptorImageInfo>();

    info = {};
    info.imageView = GetView();
    // descriptor sets are cached, so bake the layout the image will hold when the descriptor is
    // consumed (see GetVulkanImageLayout), not whatever layout it happens to be in right now
    info.imageLayout = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_LAYOUT_GENERAL
                                                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    set_write.pImageInfo = &info;
}
} // namespace sparkle

#endif
