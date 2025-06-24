#pragma once

#if ENABLE_VULKAN

#include "VulkanMemory.h"

namespace sparkle
{
class VulkanImage;

inline VkBufferUsageFlags GetBufferUsageFlags(RHIBuffer::BufferUsage usage)
{
    VkBufferUsageFlags flags = 0;
    if (usage & RHIBuffer::BufferUsage::TransferSrc)
    {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::TransferDst)
    {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::UniformBuffer)
    {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::StorageBuffer)
    {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::VertexBuffer)
    {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::IndexBuffer)
    {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::DeviceAddress)
    {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (usage & RHIBuffer::BufferUsage::AccelerationStructureBuildInput)
    {
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    if (usage & RHIBuffer::BufferUsage::AccelerationStructureStorage)
    {
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    }

    return flags;
}

class VulkanBuffer : public RHIBuffer
{
public:
    VulkanBuffer(const RHIBuffer::Attribute &attribute, const std::string &name) : RHIBuffer(attribute, name)
    {
        Create();
        if (GetUsage() & RHIBuffer::BufferUsage::DeviceAddress)
        {
            RequestDeviceAddress();
        }
    }

    ~VulkanBuffer() override;

    [[nodiscard]] VkBuffer GetResourceThisFrame() const
    {
        return GetResource();
    }

    [[nodiscard]] VkBuffer GetResource() const
    {
        if (IsDynamic())
        {
            const auto &dynamic_buffer = dynamic_allocation_.GetBuffer();
            ASSERT_F(!dynamic_buffer->IsDynamic(), "recursive dynamic buffer is not allowed!");
            return RHICast<VulkanBuffer>(dynamic_buffer)->GetResource();
        }

        return buffer_;
    }

    [[nodiscard]] VkDeviceOrHostAddressConstKHR GetDeviceAddressConst() const
    {
        ASSERT(device_address_cached_);
        return {device_address_};
    }

    [[nodiscard]] VkDeviceOrHostAddressKHR GetDeviceAddress() const
    {
        ASSERT(device_address_cached_);
        return {device_address_};
    }

    void RequestDeviceAddress();

    void *Lock() override;

    void UnLock() override;

    void CopyToBuffer(const RHIBuffer *buffer) const override;

    void CopyToImage(const RHIImage *image) const override;

    void WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                         std::vector<VkWriteDescriptorSet> &out_set_write) const;

private:
    void Create();

    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_;

    VkDeviceAddress device_address_;
    bool device_address_cached_ = false;
};
} // namespace sparkle

#endif
