#if ENABLE_VULKAN

#include "VulkanDescriptorSet.h"

#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSetManager.h"

namespace sparkle
{
void VulkanDescriptorSet::Bind(VkPipelineBindPoint bind_point, VkPipelineLayout pipeline_layout,
                               const RHIShaderResourceSet &resource_set, unsigned id)
{
    ASSERT(layout_hash_ == resource_set.GetLayoutHash());

    RequestOrUpdateDescriptorSet(resource_set);

    auto frame_index = context->GetRHI()->GetFrameIndex();

    vkCmdBindDescriptorSets(context->GetCurrentCommandBuffer(), bind_point, pipeline_layout, id, 1, &descriptor_set_,
                            static_cast<unsigned int>(dynamic_descriptor_offsets_[frame_index].size()),
                            dynamic_descriptor_offsets_[frame_index].data());
}

VulkanDescriptorSet::~VulkanDescriptorSet()
{
    if (bound_resource_hash_ != 0)
    {
        context->GetDescriptorSetManager().ReleaseDescriptorSet(bound_resource_hash_, layout_hash_);
    }
}

void VulkanDescriptorSet::RequestOrUpdateDescriptorSet(const RHIShaderResourceSet &resource_set)
{
    auto &manager = context->GetDescriptorSetManager();

    // resource hash fully reflects the identity of the descriptor set,
    // except for resource arrays which will be handled by descriptor set manager.

    auto resource_hash = resource_set.GetResourceHash();
    if (bound_resource_hash_ == resource_hash)
    {
        return;
    }

    if (bound_resource_hash_ != 0)
    {
        manager.ReleaseDescriptorSet(bound_resource_hash_, layout_hash_);
    }

    bound_resource_hash_ = resource_hash;

    descriptor_set_ = manager.RequestDescriptorSet(resource_hash, resource_set);

    // dynamic buffers share the same descriptor set, but with different offsets. we store them separately.
    dynamic_descriptor_offsets_.resize(context->GetRHI()->GetMaxFramesInFlight());
    for (auto frame_index = 0u; frame_index < context->GetRHI()->GetMaxFramesInFlight(); frame_index++)
    {
        dynamic_descriptor_offsets_[frame_index].clear();

        for (const auto *binding : resource_set.GetBindings())
        {
            if (binding->GetResource()->IsDynamic())
            {
                ASSERT_EQUAL(binding->GetType(), RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer);
                ASSERT(!binding->GetResource()->IsBindless());

                auto *bound_resource = binding->GetResource();
                const auto *buffer = RHICast<VulkanBuffer>(bound_resource);

                dynamic_descriptor_offsets_[frame_index].push_back(
                    static_cast<unsigned>(buffer->GetOffset(frame_index)));
            }
        }
    }
}
} // namespace sparkle

#endif
