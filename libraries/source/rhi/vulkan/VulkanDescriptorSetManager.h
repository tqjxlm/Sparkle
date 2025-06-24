#pragma once

#if ENABLE_VULKAN

#include "core/Hash.h"
#include "rhi/VulkanRHI.h"

#include <unordered_set>

namespace sparkle
{
class VulkanResourceArray;

inline VkDescriptorType GetVulkanDescriptorType(RHIShaderResourceReflection::ResourceType type)
{
    switch (type)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    case RHIShaderResourceReflection::ResourceType::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case RHIShaderResourceReflection::ResourceType::Texture2D:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case RHIShaderResourceReflection::ResourceType::Sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case RHIShaderResourceReflection::ResourceType::StorageImage2D:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case RHIShaderResourceReflection::ResourceType::AccelerationStructure:
        return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    UnImplemented(type);
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

class VulkanDescriptorSetManager
{
public:
    struct DescriptorSetAndBinding
    {
        VkDescriptorSet resource;
        unsigned slot;

        bool operator==(const DescriptorSetAndBinding &other) const
        {
            return resource == other.resource && slot == other.slot;
        }
    };

    struct SharedDescriptorSet
    {
        VkDescriptorSet resource;
        std::vector<size_t> bound_resource_id;
        unsigned ref_count = 0;
    };

    void Init();

    void Cleanup();

    void MarkResourceArrayDirty(VulkanResourceArray *array);

    void UpdateDirtyResourceArrays();

    ~VulkanDescriptorSetManager();

    VkDescriptorSetLayout RequestLayout(const RHIShaderResourceSet &resource_set);

    VkDescriptorSet RequestDescriptorSet(uint32_t resource_hash, const RHIShaderResourceSet &resource_set);

    void ReleaseDescriptorSet(uint32_t resource_hash, uint32_t layout_hash);

private:
    void CreateDescriptorPool();

    VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout, const RHIShaderResourceSet &resource_set);

    void UpdateDescriptorSet(VulkanDescriptorSetManager::SharedDescriptorSet &shared_descriptor_set,
                             const RHIShaderResourceSet &resource_set);

    struct DescriptorSetCache
    {
        std::vector<SharedDescriptorSet> all_sets;
        std::unordered_map<uint32_t, unsigned> allocated_sets;
        std::vector<unsigned> free_sets;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    };

    using DescriptorLayoutCache = std::unordered_map<uint32_t, DescriptorSetCache>;
    DescriptorLayoutCache cache_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    std::unordered_set<VulkanResourceArray *> dirty_resource_arrays_;
    std::unordered_map<size_t, std::unordered_set<DescriptorSetAndBinding>> resource_array_set_usage_;
};
} // namespace sparkle

namespace std
{
template <> struct hash<sparkle::VulkanDescriptorSetManager::DescriptorSetAndBinding>
{
    size_t operator()(const sparkle::VulkanDescriptorSetManager::DescriptorSetAndBinding &s) const
    {
        size_t h1 = hash<unsigned>{}(s.slot);
        size_t h2 = hash<void *>{}(s.resource);

        size_t seed = 0;
        sparkle::HashCombine(seed, h1);
        sparkle::HashCombine(seed, h2);
        return seed;
    }
};
} // namespace std

#endif
