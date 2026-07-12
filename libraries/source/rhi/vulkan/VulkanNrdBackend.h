#pragma once

#if ENABLE_VULKAN

#include "rhi/RHINrdBackend.h"

#include "VulkanImage.h"

namespace sparkle
{
// Runs NRD's SPIR-V pipelines as packed by the android shader cook (see docs/Nrd.md). Bindings are
// reflected on device with spirv_reflect; pool textures are raw Vulkan images kept in GENERAL for
// their whole life, so inter-dispatch hazards reduce to plain compute->compute memory barriers.
class VulkanNrdBackend final : public RHINrdBackend
{
public:
    VulkanNrdBackend() = default;
    ~VulkanNrdBackend() override;

    bool AddPipeline(const CookedPipeline &pipeline) override;

    [[nodiscard]] uint32_t GetPipelineCount() const override
    {
        return static_cast<uint32_t>(pipelines_.size());
    }

    void AllocateResources(uint32_t width, uint32_t height, const PoolTexture *permanent, uint32_t permanent_count,
                           const PoolTexture *transient, uint32_t transient_count, const uint32_t *samplers,
                           uint32_t sampler_count, uint32_t constant_buffer_size) override;

    void RunDispatches(const Dispatch *dispatches, uint32_t count) override;

private:
    // set is ~0u when the register was stripped as unused
    struct BindingRef
    {
        uint32_t set = ~0u;
        uint32_t binding = 0;
    };

    struct NrdPipeline
    {
        VkShaderModule shader_module = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> set_layouts;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkPipeline pso = VK_NULL_HANDLE;
        // NRD register order -> reflected descriptor slot
        std::vector<BindingRef> srv_bindings;
        std::vector<BindingRef> uav_bindings;
        std::vector<BindingRef> sampler_bindings;
        BindingRef cb_binding;
    };

    struct PoolImage
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    static PoolImage CreatePoolImage(const PoolTexture &desc, uint32_t width, uint32_t height, uint32_t index);
    static void DestroyPoolImage(PoolImage &pool_image);
    void InitializePoolLayouts(VkCommandBuffer command_buffer);

    std::vector<NrdPipeline> pipelines_;
    std::vector<PoolImage> permanent_pool_;
    std::vector<PoolImage> transient_pool_;
    std::vector<RHIResourceRef<RHISampler>> samplers_;

    RHIResourceRef<RHIBuffer> constant_buffer_;
    uint32_t constant_slot_size_ = 0;
    uint32_t constant_slot_count_ = 0;
    uint32_t constant_slot_cursor_ = 0;
    uint32_t constant_buffer_data_size_ = 0;

    std::vector<VkDescriptorPool> descriptor_pools_;

    bool pool_layouts_initialized_ = false;
};
} // namespace sparkle

#endif
