#include "VulkanResourceArray.h"

#if ENABLE_VULKAN

#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSetManager.h"
#include "VulkanImage.h"

namespace sparkle
{
void VulkanResourceArray::WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set,
                                          VkDescriptorType descriptor_type,
                                          std::vector<VkWriteDescriptorSet> &out_set_write) const
{
    auto num_resources = GetArraySize();

    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
        for (auto i = 0u; i < num_resources; i++)
        {
            const auto *buffer = RHICast<VulkanBuffer>(GetResourceAt(i));

            if (buffer)
            {
                buffer->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                out_set_write.back().dstArrayElement = i;
            }
        }

        break;
    }
    case RHIShaderResourceReflection::ResourceType::Texture2D: {
        for (auto i = 0u; i < num_resources; i++)
        {
            auto *image = RHICast<VulkanImageView>(GetResourceAt(i));
            if (image)
            {
                image->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                out_set_write.back().dstArrayElement = i;
            }
        }
        break;
    }
    default:
        UnImplemented(type_);
        break;
    }
}

void VulkanResourceArray::OnResourceUpdate()
{
    RHIResourceArray::OnResourceUpdate();

    context->GetDescriptorSetManager().MarkResourceArrayDirty(this);
}

void VulkanResourceArray::WriteDescriptorForDirtyResource(uint32_t slot, VkDescriptorSet descriptor_set,
                                                          std::vector<VkWriteDescriptorSet> &out_set_write)
{
    auto descriptor_type = GetVulkanDescriptorType(type_);
    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
        for (auto i : dirty_resource_indices_)
        {
            const auto *buffer = RHICast<VulkanBuffer>(GetResourceAt(i));

            if (buffer)
            {
                buffer->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                out_set_write.back().dstArrayElement = i;
            }
        }

        break;
    }
    case RHIShaderResourceReflection::ResourceType::Texture2D: {
        for (auto i : dirty_resource_indices_)
        {
            auto *image = RHICast<VulkanImageView>(GetResourceAt(i));
            if (image)
            {
                image->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                out_set_write.back().dstArrayElement = i;
            }
        }
        break;
    }
    default:
        UnImplemented(type_);
        break;
    }

    dirty_resource_indices_.clear();
}
} // namespace sparkle

#endif
