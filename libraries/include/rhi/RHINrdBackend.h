#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sparkle
{
class RHIImage;

// Keeps NRD's Metal detail out of the renderer: the renderer owns the nrd::Instance and hands cooked
// shaders + inputs across this seam. Obtain one via RHIContext::CreateNrdBackend().
class RHINrdBackend
{
public:
    virtual ~RHINrdBackend() = default;

    // One NRD pipeline pre-cross-compiled by the nrd_cook test case (see shaders/nrd/cooked): MSL
    // source plus the reflection data the runtime would otherwise need spirv-cross for. The index
    // vectors map NRD register order (t0.., u0.., s0..) to MSL indices; ~0u = stripped as unused.
    struct CookedPipeline
    {
        std::string msl_source;
        std::string entry_point;
        uint32_t threads_per_group[3];
        uint32_t constant_buffer_index; // ~0u if the pipeline has no constant buffer
        std::vector<uint32_t> srv_texture_indices;
        std::vector<uint32_t> uav_texture_indices;
        std::vector<uint32_t> sampler_indices;
    };

    // Appends in call order: NRD dispatches reference pipelines by that index. Returns false on failure.
    virtual bool AddPipeline(const CookedPipeline &pipeline) = 0;

    [[nodiscard]] virtual uint32_t GetPipelineCount() const = 0;

    // One NRD pool texture request. `format` carries an nrd::Format value as a plain int so this header,
    // which RHI.h includes transitively, stays free of NRD headers.
    struct PoolTexture
    {
        uint32_t format;
        uint16_t downsample_factor;
    };

    // Allocate NRD's internal texture pool (each texture sized = render resolution / its downsample), the
    // samplers (nrd::Sampler values), and the per-dispatch constant buffer. Call once after AddPipeline().
    virtual void AllocateResources(uint32_t width, uint32_t height, const PoolTexture *permanent,
                                   uint32_t permanent_count, const PoolTexture *transient,
                                   uint32_t transient_count, const uint32_t *samplers, uint32_t sampler_count,
                                   uint32_t constant_buffer_size) = 0;

    // One resource of one NRD dispatch, pre-resolved by the renderer from nrd::ResourceDesc: pool textures
    // live backend-side (referenced by index), user-facing IN_*/OUT_* textures come as engine images.
    struct DispatchResource
    {
        enum class Source : uint8_t
        {
            User,
            PermanentPool,
            TransientPool
        };
        Source source;
        bool is_uav;
        uint16_t index_in_pool;
        RHIImage *user_image;
    };

    // One nrd::DispatchDesc. `resources` follow NRD's concatenated range order (all SRVs in t0.. order,
    // then all UAVs in u0.. order) — the backend assigns registers by walking them in that order.
    struct Dispatch
    {
        uint32_t pipeline_index;
        uint32_t grid_width;
        uint32_t grid_height;
        const uint8_t *constant_data;
        uint32_t constant_size;
        const DispatchResource *resources;
        uint32_t resource_count;
    };

    // Record the frame's NRD dispatch sequence. Must run between engine compute passes (the backend opens
    // its own serial encoder on the current command buffer, which implicitly barriers between dispatches).
    virtual void RunDispatches(const Dispatch *dispatches, uint32_t count) = 0;
};
} // namespace sparkle
