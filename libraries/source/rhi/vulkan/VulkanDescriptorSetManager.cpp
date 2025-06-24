#if ENABLE_VULKAN

#include "VulkanDescriptorSetManager.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSet.h"
#include "VulkanImage.h"
#include "VulkanRayTracing.h"
#include "VulkanResourceArray.h"

namespace sparkle
{
static constexpr uint32_t MaxTotalBindlessResources = RHIShaderResourceBinding::MaxBindlessResources * 8;

struct ShaderDescriptorSetInfo
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> flags;

    void AllocateBindings(size_t new_size)
    {
        bindings.resize(new_size);
        flags.resize(new_size, 0);
    }
};

static ShaderDescriptorSetInfo CollectDescriptorSetInfo(const RHIShaderResourceSet &resource_set)
{
    ShaderDescriptorSetInfo info;

    const auto &bindings = resource_set.GetBindings();

    info.AllocateBindings(bindings.size());

    for (auto binding_id = 0u; binding_id < bindings.size(); binding_id++)
    {
        const auto *shader_binding = bindings[binding_id];
        if (!shader_binding)
        {
            ASSERT(false);
            continue;
        }

        VkDescriptorSetLayoutBinding &binding = info.bindings[binding_id];
        binding.binding = binding_id;

        if (shader_binding->IsBindless())
        {
            binding.descriptorCount = RHIShaderResourceBinding::MaxBindlessResources;
            info.flags[binding_id] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        else
        {
            binding.descriptorCount = 1;
        }

        if (shader_binding->IsBindless() ||
            shader_binding->GetType() == RHIShaderResourceReflection::ResourceType::AccelerationStructure)
        {
            info.flags[binding_id] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }

        binding.descriptorType = GetVulkanDescriptorType(shader_binding->GetType());

        // ARM best practice suggests using stage_all for all cases
        binding.stageFlags = VK_SHADER_STAGE_ALL;
    }

    return info;
}

static void CollectDescriptorUpdate(const RHIShaderResourceBinding *binding, unsigned slot,
                                    VkDescriptorSet descriptor_set, std::vector<VkWriteDescriptorSet> &out_set_write)
{
    const VkDescriptorType descriptor_type = GetVulkanDescriptorType(binding->GetType());
    auto *bound_resource = binding->GetResource();

    if (bound_resource == nullptr)
    {
        auto &set_write = out_set_write.emplace_back(VkWriteDescriptorSet{});
        InitDescriptorWrite(set_write, slot, descriptor_set, 0, descriptor_type);
    }
    else
    {
        if (binding->IsBindless())
        {
            RHICast<VulkanResourceArray>(bound_resource)
                ->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);

            // after a full update, all dirty resources should be cleared
            RHICast<VulkanResourceArray>(bound_resource)->FinishUpdate();
        }
        else
        {
            switch (binding->GetType())
            {
            case RHIShaderResourceReflection::ResourceType::UniformBuffer:
            case RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer:
            case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
                RHICast<VulkanBuffer>(bound_resource)
                    ->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Texture2D:
            case RHIShaderResourceReflection::ResourceType::StorageImage2D: {
                RHICast<VulkanImageView>(bound_resource)
                    ->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Sampler: {
                RHICast<VulkanSampler>(bound_resource)
                    ->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::AccelerationStructure: {
                RHICast<VulkanTLAS>(bound_resource)
                    ->WriteDescriptor(slot, descriptor_set, descriptor_type, out_set_write);
                break;
            }
            }
        }
    }
}

VkDescriptorSet VulkanDescriptorSetManager::AllocateDescriptorSet(VkDescriptorSetLayout layout,
                                                                  const RHIShaderResourceSet &resource_set)
{
    bool is_bindless = false;

    for (const auto *binding : resource_set.GetBindings())
    {
        ASSERT_F(binding && binding->GetResource(), "resource not bound {}", binding->GetReflection()->name);

        if (binding->GetResource()->IsBindless())
        {
            is_bindless = true;
        }
    }

    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.pNext = nullptr;
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    if (is_bindless)
    {
        // allow this descriptor to contain sub-resources
        auto &count_info =
            *context->GetRHI()->AllocateOneFrameMemory<VkDescriptorSetVariableDescriptorCountAllocateInfo>();
        count_info = {};

        count_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
        count_info.descriptorSetCount = 1;
        count_info.pDescriptorCounts = &RHIShaderResourceBinding::MaxBindlessResources;
        alloc_info.pNext = &count_info;
    }

    VkDescriptorSet descriptor_set;
    CHECK_VK_ERROR(vkAllocateDescriptorSets(context->GetDevice(), &alloc_info, &descriptor_set));

    return descriptor_set;
}

void VulkanDescriptorSetManager::UpdateDescriptorSet(
    VulkanDescriptorSetManager::SharedDescriptorSet &shared_descriptor_set, const RHIShaderResourceSet &resource_set)
{
    const auto &bindings = resource_set.GetBindings();

    std::vector<VkWriteDescriptorSet> set_writes;

    for (auto slot = 0u; slot < bindings.size(); slot++)
    {
        auto *resource = bindings[slot]->GetResource();
        auto old_id = shared_descriptor_set.bound_resource_id[slot];
        auto new_id = resource->GetId();
        const bool is_bindless = resource->IsBindless();
        const bool need_update = old_id != new_id;
        if (need_update)
        {
            shared_descriptor_set.bound_resource_id[slot] = new_id;

            CollectDescriptorUpdate(bindings[slot], slot, shared_descriptor_set.resource, set_writes);

            if (is_bindless)
            {
                resource_array_set_usage_[old_id].erase({shared_descriptor_set.resource, slot});

                resource_array_set_usage_[new_id].insert({shared_descriptor_set.resource, slot});
            }
        }
    }

    vkUpdateDescriptorSets(context->GetDevice(), static_cast<uint32_t>(set_writes.size()), set_writes.data(), 0,
                           nullptr);
}

VkDescriptorSet VulkanDescriptorSetManager::RequestDescriptorSet(uint32_t resource_hash,
                                                                 const RHIShaderResourceSet &resource_set)
{
    auto layout_hash = resource_set.GetLayoutHash();
    auto &cache = cache_[layout_hash];
    VkDescriptorSetLayout layout = cache.layout;

    ASSERT(layout);

    // according to vulkan best practice for mobile, we do not destroy allocated descriptor set
    // 1. if an existing one has the matching hash, just use it
    // 2. if no existing one can be used, find one that is not used and update it to suit current resources
    // 3. if no descriptor set is free, we have to allocate a new one

    unsigned descriptor_index;

    if (cache.allocated_sets.contains(resource_hash))
    {
        descriptor_index = cache.allocated_sets[resource_hash];
    }
    else
    {
        if (cache.free_sets.empty())
        {
            // no descriptor set is available, allocate a new one
            auto &new_set = cache.all_sets.emplace_back(SharedDescriptorSet{});
            new_set.resource = AllocateDescriptorSet(layout, resource_set);
            new_set.bound_resource_id.resize(resource_set.GetBindings().size());

            descriptor_index = static_cast<unsigned>(cache.all_sets.size() - 1);
        }
        else
        {
            // get a free descriptor set
            // TODO(tqjxlm): get the descriptor that needs least effort to update
            descriptor_index = cache.free_sets.back();
            cache.free_sets.pop_back();
        }

        cache.allocated_sets[resource_hash] = descriptor_index;

        UpdateDescriptorSet(cache.all_sets[descriptor_index], resource_set);
    }

    cache.all_sets[descriptor_index].ref_count++;
    return cache.all_sets[descriptor_index].resource;
}

void VulkanDescriptorSetManager::ReleaseDescriptorSet(uint32_t resource_hash, uint32_t layout_hash)
{
    context->GetRHI()->EnqueueEndOfFrameTasks([this, resource_hash, layout_hash]() {
        auto &cache = cache_[layout_hash];

        ASSERT(cache.allocated_sets.contains(resource_hash));

        auto index = cache.allocated_sets[resource_hash];
        cache.all_sets[index].ref_count--;

        // mark it as free
        if (cache.all_sets[index].ref_count == 0)
        {
            cache.allocated_sets.erase(resource_hash);
            cache.free_sets.push_back(index);
        }
    });
}

VulkanDescriptorSetManager::~VulkanDescriptorSetManager()
{
    Cleanup();
}

void VulkanDescriptorSetManager::Init()
{
    CreateDescriptorPool();
}

void VulkanDescriptorSetManager::Cleanup()
{
    if (descriptor_pool_ != VK_NULL_HANDLE)
    {
        Log(Debug, "VulkanDescriptorSetManager Cleanup");

        for (const auto &[hash, cache] : cache_)
        {
            ASSERT(cache.layout != VK_NULL_HANDLE);
            vkDestroyDescriptorSetLayout(context->GetDevice(), cache.layout, nullptr);
        }
        cache_.clear();

        vkDestroyDescriptorPool(context->GetDevice(), descriptor_pool_, nullptr);

        descriptor_pool_ = VK_NULL_HANDLE;
    }
}

void VulkanDescriptorSetManager::CreateDescriptorPool()
{
    std::vector<VkDescriptorPoolSize> pool_sizes = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = MaxTotalBindlessResources},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MaxTotalBindlessResources},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MaxTotalBindlessResources},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = MaxTotalBindlessResources},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = MaxTotalBindlessResources},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MaxTotalBindlessResources},
    };

    if (context->SupportsHardwareRayTracing())
    {
        pool_sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MaxTotalBindlessResources});
    }

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<unsigned>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = MaxTotalBindlessResources * static_cast<unsigned>(pool_sizes.size());
    if (context->SupportsHardwareRayTracing())
    {
        pool_info.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    }

    CHECK_VK_ERROR(vkCreateDescriptorPool(context->GetDevice(), &pool_info, nullptr, &descriptor_pool_));
}

VkDescriptorSetLayout VulkanDescriptorSetManager::RequestLayout(const RHIShaderResourceSet &resource_set)
{
    auto &layout = cache_[resource_set.GetLayoutHash()].layout;
    if (layout != VK_NULL_HANDLE)
    {
        return layout;
    }

    auto descriptor_set_info = CollectDescriptorSetInfo(resource_set);

    const auto &bindings = descriptor_set_info.bindings;
    const auto &bindless_flags = descriptor_set_info.flags;

    VkDescriptorSetLayoutCreateInfo setinfo = {};
    setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT extended_info;
    if (context->SupportsHardwareRayTracing())
    {
        extended_info = {};
        extended_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        extended_info.bindingCount = static_cast<uint32_t>(bindless_flags.size());
        extended_info.pBindingFlags = bindless_flags.data();
        setinfo.pNext = &extended_info;

        setinfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    }

    setinfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setinfo.pBindings = bindings.data();

    vkCreateDescriptorSetLayout(context->GetDevice(), &setinfo, nullptr, &layout);

    return layout;
}

void VulkanDescriptorSetManager::UpdateDirtyResourceArrays()
{
    std::vector<VkWriteDescriptorSet> set_writes;
    for (auto *dirty_array : dirty_resource_arrays_)
    {
        auto resource_id = dirty_array->GetId();
        if (!resource_array_set_usage_.contains(resource_id))
        {
            // this array has never been used
            continue;
        }

        for (const auto &usage : resource_array_set_usage_[resource_id])
        {
            dirty_array->WriteDescriptorForDirtyResource(usage.slot, usage.resource, set_writes);
        }

        dirty_array->FinishUpdate();
    }

    dirty_resource_arrays_.clear();

    if (!set_writes.empty())
    {
        vkUpdateDescriptorSets(context->GetDevice(), static_cast<uint32_t>(set_writes.size()), set_writes.data(), 0,
                               nullptr);
    }
}

void VulkanDescriptorSetManager::MarkResourceArrayDirty(VulkanResourceArray *array)
{
    dirty_resource_arrays_.insert(array);
}
} // namespace sparkle

#endif
