#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHINrdBackend.h"

#import <Metal/Metal.h>

#include <vector>

namespace sparkle
{
class MetalNrdBackend final : public RHINrdBackend
{
public:
    explicit MetalNrdBackend(id<MTLDevice> device);

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
    struct NrdPipeline
    {
        id<MTLComputePipelineState> pso;
        MTLSize threads_per_group; // SPIR-V LocalSize (NRD grid sizes are in threadgroups)
        std::vector<uint32_t> srv_texture_indices;
        std::vector<uint32_t> uav_texture_indices;
        std::vector<uint32_t> sampler_indices;
        uint32_t constant_buffer_index = ~0u;
    };

    id<MTLTexture> CreatePoolTexture(const PoolTexture &desc, uint32_t width, uint32_t height);

    id<MTLDevice> device_;
    MTLCompileOptions *compile_options_;
    std::vector<NrdPipeline> pipelines_;
    std::vector<id<MTLTexture>> permanent_pool_;
    std::vector<id<MTLTexture>> transient_pool_;
    std::vector<id<MTLSamplerState>> samplers_;

    // Per-dispatch constants suballocated from a shared ring: the GPU reads each region at execution time,
    // so every dispatch (across in-flight frames) needs its own slot. Sized for ~10 frames of ReBLUR.
    id<MTLBuffer> constant_buffer_ = nil;
    uint32_t constant_slot_size_ = 0;
    uint32_t constant_slot_count_ = 0;
    uint32_t constant_slot_cursor_ = 0;
};
} // namespace sparkle

#endif
