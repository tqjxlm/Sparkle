#if ENABLE_VULKAN

#include "VulkanBuffer.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSet.h"
#include "VulkanImage.h"

namespace sparkle
{
void VulkanBuffer::RequestDeviceAddress()
{
    ASSERT(GetUsage() & RHIBuffer::BufferUsage::DeviceAddress);

    if (device_address_cached_)
    {
        return;
    }

    const VkBufferDeviceAddressInfoKHR info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer_};

    device_address_ = vkGetBufferDeviceAddressKHR(context->GetDevice(), &info);
    device_address_cached_ = true;
}

void VulkanBuffer::Create()
{
    if (attribute_.is_dynamic)
    {
        // we will not create a real buffer in this case since it will be re-generate every frame
        dynamic_allocation_ = context->GetRHI()->GetBufferManager()->SubAllocateDynamicBuffer(attribute_);
    }
    else
    {
        const VkBufferUsageFlags vk_usage_flags = GetBufferUsageFlags(GetUsage());
        const VkMemoryPropertyFlags vk_property_flags = GetVulkanMemoryPropertyFlags(GetMemoryProperty());

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = GetSize();
        buffer_info.usage = vk_usage_flags;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo vma_alloc_info{};
        vma_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        vma_alloc_info.requiredFlags = vk_property_flags;

        if (GetMemoryProperty() & RHIMemoryProperty::HostVisible)
        {
            vma_alloc_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        }

        bool always_map = GetMemoryProperty() & RHIMemoryProperty::AlwaysMap;

        if (always_map)
        {
            vma_alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo alloc_info;
        CHECK_VK_ERROR(vmaCreateBuffer(context->GetMemoryAllocator(), &buffer_info, &vma_alloc_info, &buffer_,
                                       &allocation_, &alloc_info));

        if (always_map)
        {
            mapped_address_ = static_cast<uint8_t *>(alloc_info.pMappedData);
        }

        context->SetDebugInfo(reinterpret_cast<uint64_t>(buffer_), VK_OBJECT_TYPE_BUFFER, GetName().c_str());
    }
}

void VulkanBuffer::CopyToBuffer(const RHIBuffer *buffer) const
{
    const auto *dst_buffer = RHICast<VulkanBuffer>(buffer);

    auto frame_index = context->GetRHI()->GetFrameIndex();

    // copy to dynamic buffer is not supported for now
    ASSERT(!dst_buffer->IsDynamic());

    VkBufferCopy copy_region{};
    copy_region.srcOffset = GetOffset(frame_index);
    copy_region.dstOffset = dst_buffer->GetOffset(UINT_MAX);
    copy_region.size = GetSize();

    ASSERT_EQUAL(GetSize(), buffer->GetSize());

    vkCmdCopyBuffer(context->GetCurrentCommandBuffer(), GetResourceThisFrame(), dst_buffer->GetResourceThisFrame(), 1,
                    &copy_region);
}

void VulkanBuffer::CopyToImage(const RHIImage *image) const
{
    auto frame_index = context->GetRHI()->GetFrameIndex();

    VkBuffer buffer_resource = GetResourceThisFrame();
    VkImage image_resource = RHICast<VulkanImage>(image)->GetImage();
    auto offset = GetOffset(frame_index);

    const auto &image_attributes = image->GetAttributes();

    uint32_t copied_bytes = 0;
    std::vector<VkBufferImageCopy> copy_regions(image_attributes.mip_levels);

    unsigned num_layers = image_attributes.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    for (auto mip_level = 0u; mip_level < image_attributes.mip_levels; mip_level++)
    {
        auto &copy_region = copy_regions[mip_level];
        copy_region = {};

        copy_region.bufferOffset = offset + copied_bytes;
        copy_region.bufferRowLength = 0;
        copy_region.bufferImageHeight = 0;

        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.mipLevel = mip_level;
        copy_region.imageSubresource.baseArrayLayer = 0;
        copy_region.imageSubresource.layerCount = num_layers;

        copy_region.imageOffset = {.x = 0, .y = 0, .z = 0};
        copy_region.imageExtent = {
            .width = image->GetWidth(mip_level), .height = image->GetHeight(mip_level), .depth = 1};

        copied_bytes += image->GetStorageSize(mip_level) * num_layers;
    }

    vkCmdCopyBufferToImage(context->GetCurrentCommandBuffer(), buffer_resource, image_resource,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copy_regions.size()),
                           copy_regions.data());
}

VulkanBuffer::~VulkanBuffer()
{
    if (buffer_)
    {
        vmaDestroyBuffer(context->GetMemoryAllocator(), buffer_, allocation_);
    }
    else if (IsDynamic())
    {
        dynamic_allocation_.Deallocate();
    }
}

void VulkanBuffer::UnLock()
{
    if (GetMemoryProperty() & RHIMemoryProperty::HostCoherent)
    {
        vmaFlushAllocation(context->GetMemoryAllocator(), allocation_, 0, GetSize());
    }
    vmaUnmapMemory(context->GetMemoryAllocator(), allocation_);
}

void *VulkanBuffer::Lock()
{
    void *data;
    vmaMapMemory(context->GetMemoryAllocator(), allocation_, &data);
    return data;
}

void VulkanBuffer::WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                                   std::vector<VkWriteDescriptorSet> &out_set_write) const
{
    auto &set_write = out_set_write.emplace_back(VkWriteDescriptorSet{});
    InitDescriptorWrite(set_write, slot, descriptor_set, 0, descriptor_type);

    auto &info = *context->GetRHI()->AllocateOneFrameMemory<VkDescriptorBufferInfo>();
    info = {};

    info.buffer = GetResource();
    // for dynamic buffers, offset is provided when binding descriptor set
    info.offset = IsDynamic() ? 0 : GetOffset(UINT_MAX);
    info.range = GetSize();

    set_write.pBufferInfo = &info;
}
} // namespace sparkle

#endif
