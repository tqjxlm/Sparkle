#pragma once

#include "core/math/Types.h"
#include "rhi/RHIResource.h"

#include <ffx_interface.h>

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace sparkle
{
class RHIBuffer;
class RHICommandContext;
class RHIComputePass;
class RHIContext;
class RHIImage;
class RHIPipelineState;
class RHIShader;
class RHIShaderResourceBinding;

// One compute pass of a FidelityFX effect, compiled by the engine's shader build (shaders/fsr). The effect's blob
// lookup hands it to FfxBackend through FfxShaderBlob::data; a pass the engine does not compile has no source.
struct FfxShaderSource
{
    RHIResourceRef<RHIShader> (*create)(RHIContext *rhi);
    Vector3UInt thread_group_size;
};

// Executes FidelityFX effects through the engine's RHI, so they run on every RHI backend. The effect's resources are
// RHI images, its pipelines are engine shaders whose resource tables carry the effect's binding names, and it records
// into the RHICommandContext it receives as its FfxCommandList, inside a render graph External pass. Every image an
// effect job touches is transitioned to the layout of that use, which leaves a sampled image Sampled and a storage
// image StorageReadWrite: the pass declares the images the render graph tracks with exactly those accesses.
class FfxBackend
{
public:
    explicit FfxBackend(RHIContext *rhi);

    ~FfxBackend();

    FfxBackend(const FfxBackend &) = delete;
    FfxBackend &operator=(const FfxBackend &) = delete;

    [[nodiscard]] FfxInterface &GetInterface()
    {
        return interface_;
    }

    // an engine image as an effect resource, registered for the dispatch it is passed to
    [[nodiscard]] static FfxApiResource Wrap(RHIImage *image);

    // an image for an effect resource description, holding its initial data; null for a type or format the RHI lacks
    [[nodiscard]] RHIResourceRef<RHIImage> CreateImage(const FfxCreateResourceDescription &description) const;

    // false once a job could not be executed
    [[nodiscard]] bool IsHealthy() const
    {
        return healthy_;
    }

private:
    struct Resource
    {
        RHIResourceRef<RHIImage> owned;
        RHIImage *image = nullptr;
        FfxApiResourceDescription description{};
    };

    struct Pipeline
    {
        RHIResourceRef<RHIShader> shader;
        RHIResourceRef<RHIPipelineState> state;
        RHIResourceRef<RHIComputePass> pass;
        Vector3UInt thread_group_size;
        // in the order the pipeline state reports them to the effect
        std::vector<RHIShaderResourceBinding *> srv_textures;
        std::vector<RHIShaderResourceBinding *> uav_textures;
        std::vector<RHIShaderResourceBinding *> constant_buffer_bindings;
        std::vector<RHIResourceRef<RHIBuffer>> constant_buffers;
    };

    // a clear of one mip of one resource to one value. its value buffer holds one value per frame, so two clears of a
    // frame share an entry only when they write the same value
    struct ClearPipeline
    {
        RHIResourceRef<RHIPipelineState> state;
        RHIResourceRef<RHIComputePass> pass;
        RHIResourceRef<RHIBuffer> value;
    };

    static FfxBackend &Get(FfxInterface *backend_interface);

    static FfxErrorCode CreateResource(FfxInterface *backend_interface, const FfxCreateResourceDescription *description,
                                       FfxUInt32 effect_context_id, FfxResourceInternal *out_resource);
    static FfxErrorCode RegisterResource(FfxInterface *backend_interface, const FfxApiResource *resource,
                                         FfxUInt32 effect_context_id, FfxResourceInternal *out_resource);
    static FfxApiResource GetApiResource(FfxInterface *backend_interface, FfxResourceInternal resource);
    static FfxErrorCode UnregisterResources(FfxInterface *backend_interface, FfxCommandList command_list,
                                            FfxUInt32 effect_context_id);
    static FfxApiResourceDescription GetResourceDescription(FfxInterface *backend_interface,
                                                            FfxResourceInternal resource);
    static FfxErrorCode DestroyResource(FfxInterface *backend_interface, FfxResourceInternal resource,
                                        FfxUInt32 effect_context_id);
    static FfxErrorCode StageConstantBufferData(FfxInterface *backend_interface, void *data, FfxUInt32 size,
                                                FfxConstantBuffer *constant_buffer);
    static FfxErrorCode CreatePipeline(FfxInterface *backend_interface, FfxShaderBlob *shader_blob,
                                       const FfxPipelineDescription *description, FfxUInt32 effect_context_id,
                                       FfxPipelineState *out_pipeline);
    static FfxErrorCode DestroyPipeline(FfxInterface *backend_interface, FfxPipelineState *pipeline,
                                        FfxUInt32 effect_context_id);
    static FfxErrorCode ScheduleGpuJob(FfxInterface *backend_interface, const FfxGpuJobDescription *job);
    static FfxErrorCode ExecuteGpuJobs(FfxInterface *backend_interface, FfxCommandList command_list,
                                       FfxUInt32 effect_context_id);

    void Dispatch(RHICommandContext &command_context, const FfxComputeJobDescription &job);

    void Clear(RHICommandContext &command_context, const FfxClearFloatJobDescription &job);

    ClearPipeline &GetClearPipeline(RHIImage *image, uint32_t mip, const std::array<float, 4> &value);

    [[nodiscard]] Resource &Find(FfxResourceInternal resource);

    RHIContext *rhi_;
    FfxInterface interface_{};

    // indexed by FfxResourceInternal::internalIndex. index 0 stays null, the effect's unset resource; created resources
    // follow, then the ones registered for the current dispatch
    std::vector<Resource> resources_;
    size_t created_resource_count_ = 1;

    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    std::vector<FfxGpuJobDescription> jobs_;
    // the constant data staged for the current dispatch
    std::deque<std::vector<uint32_t>> staged_constants_;
    // keyed by image id, mip and value
    std::map<std::tuple<size_t, uint32_t, std::array<float, 4>>, ClearPipeline> clear_pipelines_;
    bool healthy_ = true;
};
} // namespace sparkle
