#include "renderer/upscaler/FsrUpscaler.h"

#include "core/Logger.h"
#include "renderer/upscaler/FfxBackend.h"
#include "rhi/RHI.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIShader.h"

#include <ffx_fsr3upscaler.h>
#include <ffx_fsr3upscaler_private.h>
#include <ffx_fsr3upscaler_shaderblobs.h>

#include <cmath>
#include <utility>

namespace sparkle
{
// the FSR 3.1 upscaler passes the engine compiles, with every resource their HLSL declares
class Fsr3UpscalerPrepareInputsShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerPrepareInputsShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_prepare_inputs.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_input_depth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_color_jittered, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_farthest_depth, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_current_luma, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_reconstructed_previous_nearest_depth,
                        RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_dilated_depth, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_dilated_motion_vectors, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerLumaPyramidShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerLumaPyramidShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_luma_pyramid.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(cbSPD, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_farthest_depth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_farthest_depth_mip1, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_current_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_frame_info, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip0, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip1, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip2, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip3, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip4, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip5, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_global_atomic, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerShadingChangePyramidShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerShadingChangePyramidShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_shading_change_pyramid.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(cbSPD, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_current_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_previous_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_dilated_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_exposure, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_spd_mip0, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip1, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip2, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip3, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip4, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_mip5, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_spd_global_atomic, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerShadingChangeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerShadingChangeShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_shading_change.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_spd_mips, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_shading_change, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerPrepareReactivityShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerPrepareReactivityShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_prepare_reactivity.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_reactive_mask, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_transparency_and_composition_mask, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_accumulation, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_accumulation, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_shading_change, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_current_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_new_locks, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_reconstructed_previous_nearest_depth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_dilated_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_dilated_depth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_exposure, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_dilated_reactive_masks, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerLumaInstabilityShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerLumaInstabilityShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_luma_instability.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(rw_luma_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_luma_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_farthest_depth_mip1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_current_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_luma_instability, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_dilated_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_exposure, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_dilated_reactive_masks, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_frame_info, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE
};

class Fsr3UpscalerAccumulateShader : public RHIShaderInfo
{
    REGISTGER_SHADER(Fsr3UpscalerAccumulateShader, RHIShaderStage::Compute,
                     "shaders/fsr/fsr3upscaler_accumulate.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(cbFSR3Upscaler, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(s_PointClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(s_LinearClamp, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(r_input_color_jittered, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_internal_upscaled_color, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_internal_upscaled_color, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(rw_upscaled_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_farthest_depth_mip1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_current_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_luma_instability, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(rw_new_locks, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(r_dilated_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_input_exposure, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_lanczos_lut, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(r_dilated_reactive_masks, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE
};

namespace
{
// the engine compiles each pass once, with the options of shaders/include/fsr3upscaler.h.slang. the context flags and
// device capabilities FfxBackend reports select exactly that permutation
constexpr uint32_t ContextFlags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
constexpr uint32_t CompiledPermutation =
    FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT | FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;

class FsrPrepareDepthShader : public RHIShaderInfo
{
    REGISTGER_SHADER(FsrPrepareDepthShader, RHIShaderStage::Compute, "shaders/fsr/fsr_prepare_depth.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(normalViewDepth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(motionHitMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDepth, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Mat4 projection;
        Vector2UInt resolution;
    };
};

const Vector3UInt PrepareDepthGroupSize{16u, 16u, 1u};

template <class Shader> RHIResourceRef<RHIShader> CreateFsrShader(RHIContext *rhi)
{
    return rhi->CreateShader<Shader>();
}

const Vector3UInt PixelGroupSize{8u, 8u, 1u};
const Vector3UInt PyramidGroupSize{256u, 1u, 1u};

const FfxShaderSource *FindShaderSource(FfxFsr3UpscalerPass pass)
{
    static const FfxShaderSource PrepareInputs{CreateFsrShader<Fsr3UpscalerPrepareInputsShader>, PixelGroupSize};
    static const FfxShaderSource LumaPyramid{CreateFsrShader<Fsr3UpscalerLumaPyramidShader>, PyramidGroupSize};
    static const FfxShaderSource ShadingChangePyramid{CreateFsrShader<Fsr3UpscalerShadingChangePyramidShader>,
                                                      PyramidGroupSize};
    static const FfxShaderSource ShadingChange{CreateFsrShader<Fsr3UpscalerShadingChangeShader>, PixelGroupSize};
    static const FfxShaderSource PrepareReactivity{CreateFsrShader<Fsr3UpscalerPrepareReactivityShader>,
                                                   PixelGroupSize};
    static const FfxShaderSource LumaInstability{CreateFsrShader<Fsr3UpscalerLumaInstabilityShader>, PixelGroupSize};
    static const FfxShaderSource Accumulate{CreateFsrShader<Fsr3UpscalerAccumulateShader>, PixelGroupSize};

    switch (pass)
    {
    case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS:
        return &PrepareInputs;
    case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID:
        return &LumaPyramid;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID:
        return &ShadingChangePyramid;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE:
        return &ShadingChange;
    case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:
        return &PrepareReactivity;
    case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY:
        return &LumaInstability;
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
        return &Accumulate;
    // sharpening, reactive mask generation and the debug view are never dispatched
    default:
        return nullptr;
    }
}
} // namespace

static FfxErrorCode GetFsrShaderBlob(FfxFsr3UpscalerPass pass, uint32_t permutation, FfxShaderBlob *blob)
{
    *blob = {};
    const FfxShaderSource *source = FindShaderSource(pass);
    if (source && permutation != CompiledPermutation)
    {
        Log(Error, "FSR: pass {} requests permutation {:#x}, the engine compiles {:#x}", static_cast<uint32_t>(pass),
            permutation, CompiledPermutation);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    blob->data = reinterpret_cast<const uint8_t *>(source);
    return FFX_OK;
}
} // namespace sparkle

// the FSR3 upscaler's blob lookup: the engine's shaders stand in for the SDK's precompiled permutations
FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass passId, uint32_t permutationOptions,
                                                   FfxShaderBlob *outBlob)
{
    return sparkle::GetFsrShaderBlob(passId, permutationOptions, outBlob);
}

namespace sparkle
{
FsrUpscaler::FsrUpscaler(RHIContext *rhi, UpscalerDesc desc) : rhi_(rhi), desc_(std::move(desc))
{
    depth_shader_ = rhi_->CreateShader<FsrPrepareDepthShader>();
    depth_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "FsrPrepareDepthPipeline");
    depth_pipeline_->SetShader<RHIShaderStage::Compute>(depth_shader_);
    depth_pipeline_->Compile();
    depth_ubo_ = rhi_->CreateBuffer({.size = sizeof(FsrPrepareDepthShader::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::None,
                                     .is_dynamic = true},
                                    "FsrPrepareDepthUBO");
    depth_pipeline_->GetShaderResource<FsrPrepareDepthShader>()->ubo().BindResource(depth_ubo_);
    depth_pass_ = rhi_->CreateComputePass("FsrPrepareDepthPass", false);

    Create();
}

FsrUpscaler::~FsrUpscaler()
{
    Destroy();
}

void FsrUpscaler::Destroy()
{
    if (context_)
    {
        ffxFsr3UpscalerContextDestroy(context_.get());
        context_.reset();
    }
    backend_.reset();
    ready_ = false;
}

void FsrUpscaler::Create()
{
    Destroy();

    backend_ = std::make_unique<FfxBackend>(rhi_);
    context_ = std::make_unique<FfxFsr3UpscalerContext>();
    FfxFsr3UpscalerContextDescription description{};
    description.flags = ContextFlags;
    description.maxRenderSize = {desc_.input_size.x(), desc_.input_size.y()};
    description.maxUpscaleSize = {desc_.output_size.x(), desc_.output_size.y()};
    description.backendInterface = backend_->GetInterface();
    if (ffxFsr3UpscalerContextCreate(context_.get(), &description) != FFX_OK)
    {
        Log(Error, "FSR: context creation failed for {}x{} -> {}x{}", desc_.input_size.x(), desc_.input_size.y(),
            desc_.output_size.x(), desc_.output_size.y());
        context_.reset();
        backend_.reset();
        return;
    }

    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    ffxFsr3UpscalerGetSharedResourceDescriptions(context_.get(), &shared);
    dilated_depth_ = backend_->CreateImage(shared.dilatedDepth);
    dilated_motion_vectors_ = backend_->CreateImage(shared.dilatedMotionVectors);
    reconstructed_previous_nearest_depth_ = backend_->CreateImage(shared.reconstructedPrevNearestDepth);

    const auto create_image = [this](PixelFormat format, const Vector2UInt &size, const char *name) {
        return rhi_->CreateImage(
            {
                .format = format,
                .width = size.x(),
                .height = size.y(),
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
                .mip_levels = 1,
                .msaa_samples = 1,
            },
            name);
    };
    depth_ = create_image(PixelFormat::R32Float, desc_.input_size, "FsrDepth");
    output_ = create_image(desc_.output_format, desc_.output_size, "FsrOutput");
    depth_pipeline_->GetShaderResource<FsrPrepareDepthShader>()->outDepth().BindResource(depth_->GetDefaultView(rhi_));

    ready_ = true;
    Log(Info, "FSR: upscaler ready, input [{} x {}], output [{} x {}]", desc_.input_size.x(), desc_.input_size.y(),
        desc_.output_size.x(), desc_.output_size.y());
}

void FsrUpscaler::Resize(const Vector2UInt &input_size, const Vector2UInt &output_size)
{
    desc_.input_size = input_size;
    desc_.output_size = output_size;
    Create();
}

RGTexture FsrUpscaler::AddTo(RenderGraph &graph, const UpscalerInputs &inputs)
{
    ASSERT(ready_);

    const auto output = graph.Import("FsrOutput", output_);
    graph.AddExternalPass("Fsr", [this, inputs, output](RGBuilder &builder) {
        for (const auto input : {inputs.color, inputs.normal_view_depth, inputs.motion_hit_metallic})
        {
            builder.Sampled(input, RHIShaderStageMask::Compute);
        }
        builder.StorageReadWrite(output, RHIShaderStageMask::Compute);
        return [this, inputs](RGExternalContext &context) { Encode(context, inputs); };
    });
    return output;
}

void FsrUpscaler::Encode(RGExternalContext &context, const UpscalerInputs &inputs)
{
    auto &command_context = context.GetCommandContext();
    const auto &size = desc_.input_size;
    RHIImage *motion = context.GetImage(inputs.motion_hit_metallic);

    auto *depth_resources = depth_pipeline_->GetShaderResource<FsrPrepareDepthShader>();
    depth_resources->normalViewDepth().BindResource(context.GetImage(inputs.normal_view_depth)->GetDefaultView(rhi_));
    depth_resources->motionHitMetallic().BindResource(motion->GetDefaultView(rhi_));
    const FsrPrepareDepthShader::UniformBufferData depth_ubo{.projection = frame_.projection, .resolution = size};
    depth_ubo_->Upload(rhi_, &depth_ubo);
    depth_->Transition(command_context, {.target_layout = RHIImageLayout::StorageWrite,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    command_context.BeginComputePass(depth_pass_);
    command_context.DispatchCompute(depth_pipeline_, {size.x(), size.y(), 1u}, PrepareDepthGroupSize);
    command_context.EndComputePass(depth_pass_);

    FfxFsr3UpscalerDispatchDescription dispatch{};
    dispatch.commandList = &command_context;
    dispatch.color = FfxBackend::Wrap(context.GetImage(inputs.color));
    dispatch.depth = FfxBackend::Wrap(depth_.get());
    // the guides' motion vectors run from the current to the previous position in uv, which FSR scales to pixels
    dispatch.motionVectors = FfxBackend::Wrap(motion);
    dispatch.dilatedDepth = FfxBackend::Wrap(dilated_depth_.get());
    dispatch.dilatedMotionVectors = FfxBackend::Wrap(dilated_motion_vectors_.get());
    dispatch.reconstructedPrevNearestDepth = FfxBackend::Wrap(reconstructed_previous_nearest_depth_.get());
    dispatch.output = FfxBackend::Wrap(output_.get());
    // FSR places a pixel's sample at its center minus the jitter
    dispatch.jitterOffset = {-frame_.jitter.x(), -frame_.jitter.y()};
    dispatch.motionVectorScale = {static_cast<float>(size.x()), static_cast<float>(size.y())};
    dispatch.renderSize = {size.x(), size.y()};
    dispatch.upscaleSize = {desc_.output_size.x(), desc_.output_size.y()};
    dispatch.frameTimeDelta = frame_.delta_time * 1000.f;
    dispatch.preExposure = 1.f;
    dispatch.reset = reset_history_;
    dispatch.cameraNear = frame_.near_plane;
    dispatch.cameraFar = frame_.far_plane;
    dispatch.cameraFovAngleVertical = 2.f * std::atan(1.f / std::abs(frame_.projection(1, 1)));
    dispatch.viewSpaceToMetersFactor = 1.f;

    if (ffxFsr3UpscalerContextDispatch(context_.get(), &dispatch) != FFX_OK || !backend_->IsHealthy())
    {
        Log(Error, "FSR: dispatch failed; selecting a fallback");
        ready_ = false;
    }
    reset_history_ = false;
}

void FsrUpscaler::UpdateFrameData(const UpscalerFrameData &frame)
{
    frame_ = frame;
    reset_history_ = reset_history_ || frame.reset_history;
}
} // namespace sparkle
