#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
inline void InitDescriptorWrite(VkWriteDescriptorSet &set_write, unsigned slot, VkDescriptorSet descriptor_set,
                                unsigned index, VkDescriptorType type)
{
    set_write = {};
    set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    set_write.dstBinding = slot;
    set_write.dstSet = descriptor_set;
    set_write.dstArrayElement = index;
    set_write.descriptorCount = 1;
    set_write.descriptorType = type;
}

class VulkanDescriptorSet
{
public:
    VulkanDescriptorSet() = default;

    VulkanDescriptorSet(VulkanDescriptorSet &&other) noexcept
    {
        bound_resource_hash_ = other.bound_resource_hash_;
        layout_hash_ = other.layout_hash_;
        std::swap(dynamic_descriptor_offsets_, other.dynamic_descriptor_offsets_);
        descriptor_set_ = other.descriptor_set_;

        other.bound_resource_hash_ = 0;
        other.layout_hash_ = 0;
    }

    ~VulkanDescriptorSet();

    void SetLayoutHash(uint32_t layout_hash)
    {
        ASSERT(layout_hash_ == 0);
        layout_hash_ = layout_hash;
    }

    void Bind(VkPipelineBindPoint bind_point, VkPipelineLayout pipeline_layout,
              const RHIShaderResourceSet &resource_set, unsigned id);

    [[nodiscard]] bool IsDynamic() const
    {
        return dynamic_descriptor_offsets_.empty();
    }

protected:
    // if we don't have a matching ds for the given resource set, request a new one
    void RequestOrUpdateDescriptorSet(const RHIShaderResourceSet &resource_set);

    VkDescriptorSet descriptor_set_ = nullptr;

    // for dynamic resources, every dynamic resource has its own offset
    std::vector<std::vector<unsigned>> dynamic_descriptor_offsets_;

    uint32_t bound_resource_hash_ = 0;

    uint32_t layout_hash_ = 0;
};
} // namespace sparkle

#endif
