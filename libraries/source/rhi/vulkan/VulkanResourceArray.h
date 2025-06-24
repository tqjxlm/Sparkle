#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "rhi/RHIResourceArray.h"

namespace sparkle
{
class VulkanResourceArray : public RHIResourceArray
{
public:
    using RHIResourceArray::RHIResourceArray;

    void WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                         std::vector<VkWriteDescriptorSet> &out_set_write) const;

    void WriteDescriptorForDirtyResource(uint32_t slot, VkDescriptorSet descriptor_set,
                                         std::vector<VkWriteDescriptorSet> &out_set_write);

    void OnResourceUpdate() override;
};
} // namespace sparkle

#endif
