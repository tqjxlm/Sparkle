#include "renderer/nrd/NrdDenoiser.h"

#include "renderer/denoiser/DenoiserHandoff.h"
#include "renderer/nrd/NrdCookedShaders.h"
#include "rhi/RHI.h"

#include <NRD.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace sparkle
{
namespace
{
constexpr float DemodEps = 0.04f;
constexpr float SkyViewZ = 1e6f;
constexpr float MotionDisplayScale = 20.f;

// must match the ReblurHitDistanceParameters fed to NRD (SetDenoiserSettings) — the pack shader
// normalizes hit distances with the same constants ReBLUR denormalizes with.
constexpr float HitDistA = 3.0f;
constexpr float HitDistB = 0.1f;
constexpr float HitDistC = 20.0f;

void ToLayout(RHICommandContext &command_context, const RHIResourceRef<RHIImage> &image, RHIImageLayout layout,
              RHIPipelineStage after, RHIPipelineStage before)
{
    image->Transition(command_context, {.target_layout = layout, .after_stage = after, .before_stage = before});
}

void CopyMatrix(float (&dst)[16], const Mat4 &src)
{
    std::memcpy(dst, src.data(), sizeof(dst));
}
} // namespace

class NrdPackShader : public RHIShaderInfo
{
    REGISTGER_SHADER(NrdPackShader, RHIShaderStage::Compute, "shaders/nrd/nrd_pack.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(gRadiance, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gRadianceSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gNormalDepth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gAlbedoObj, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gMotion, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gSpecAlbedo, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outMv, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outNormalRoughness, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outViewZ, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outDiff, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpec, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float hit_dist_a;
        float hit_dist_b;
        float hit_dist_c;
        float demod_eps;
        float sky_view_z;
    };
};

class NrdResolveShader : public RHIShaderInfo
{
    REGISTGER_SHADER(NrdResolveShader, RHIShaderStage::Compute, "shaders/nrd/nrd_resolve.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(denoisedDiff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(denoisedSpec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inMv, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inDiff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gRadiance, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gAlbedoObj, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gMotion, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(gSpecAlbedo, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outputImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(validation, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(sceneAccum, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outputHistory, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        uint32_t mode;
        float demod_eps;
        float view_z_scale;
        float motion_scale;
        float handoff_weight;
        float stabilization_beta;
    };
};

NrdDenoiser::NrdDenoiser(RHIContext *rhi, const DenoiserDesc &desc)
    : rhi_(rhi), input_size_(desc.input_size), timings_(rhi, "NrdPerf", desc.max_frames_in_flight)
{
    ASSERT(rhi_);
    ASSERT(desc.max_frames_in_flight > 0);
    SampleConfig();
    Initialize(desc.accumulator_format);
}

void NrdDenoiser::SampleConfig()
{
    const NrdConfig &live = NrdConfig::Get();
    config_ = {.stabilization = live.stabilization, .debug_mode = live.debug_mode};
}

NrdDenoiser::~NrdDenoiser()
{
    if (timings_.HasSamples())
    {
        timings_.LogTimings("final");
    }

    if (instance_)
    {
        nrd::DestroyInstance(*instance_);
    }
}

RHIResourceRef<RHIImage> NrdDenoiser::CreateFullScreenTexture(PixelFormat format, const std::string &name) const
{
    auto image = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = format,
            .width = input_size_.x(),
            .height = input_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        name);

    ToLayout(*rhi_->GetCommandContext(), image, RHIImageLayout::Read, RHIPipelineStage::Top,
             RHIPipelineStage::ComputeShader);

    return image;
}

void NrdDenoiser::Initialize(PixelFormat output_format)
{
    // failures latch permanently (IsActive() -> false): they are deterministic, and retrying every
    // frame would re-run the full SPIRV->MSL pipeline compilation and leak nrd instances
    if (enabled_resources_ready_ || enabled_resources_failed_)
    {
        return;
    }

    backend_ = rhi_->CreateNrdBackend();
    if (!backend_)
    {
        Log(Error, "NRD: this RHI has no NRD backend; denoiser stays disabled");
        enabled_resources_failed_ = true;
        return;
    }

    nrd::DenoiserDesc denoiser{};
    denoiser.identifier = 0;
    denoiser.denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = &denoiser;
    creation.denoisersNum = 1;
    if (nrd::CreateInstance(creation, instance_) != nrd::Result::SUCCESS)
    {
        Log(Error, "NRD: CreateInstance(REBLUR_DIFFUSE_SPECULAR) failed");
        enabled_resources_failed_ = true;
        return;
    }

    const nrd::InstanceDesc &desc = *nrd::GetInstanceDesc(*instance_);
    const nrd::LibraryDesc &lib = *nrd::GetLibraryDesc();

    NrdCookedShaders cooked;
    if (!cooked.Load())
    {
        enabled_resources_failed_ = true;
        return;
    }
    if (cooked.VersionMajor() != lib.versionMajor || cooked.VersionMinor() != lib.versionMinor ||
        cooked.VersionBuild() != lib.versionBuild)
    {
        Log(Error, "NRD: cooked shaders are stale ({}.{}.{} vs NRD {}.{}.{}); rebuild to re-run the shader cook",
            cooked.VersionMajor(), cooked.VersionMinor(), cooked.VersionBuild(), lib.versionMajor, lib.versionMinor,
            lib.versionBuild);
        enabled_resources_failed_ = true;
        return;
    }

    uint32_t ok = 0;
    for (uint32_t i = 0; i < desc.pipelinesNum; i++)
    {
        RHINrdBackend::CookedPipeline pipeline;
        if (cooked.BuildPipeline(desc.pipelines[i].shaderIdentifier, pipeline) && backend_->AddPipeline(pipeline))
        {
            ok++;
        }
    }

    Log(Info, "NRD: pipelines {}/{} created; pool {}+{} textures, cb {}B", ok, desc.pipelinesNum,
        desc.permanentPoolSize, desc.transientPoolSize, desc.constantBufferMaxDataSize);
    if (ok != desc.pipelinesNum)
    {
        enabled_resources_failed_ = true;
        return;
    }

    std::vector<RHINrdBackend::PoolTexture> permanent(desc.permanentPoolSize);
    for (uint32_t i = 0; i < desc.permanentPoolSize; i++)
    {
        permanent[i] = {.format = static_cast<uint32_t>(desc.permanentPool[i].format),
                        .downsample_factor = desc.permanentPool[i].downsampleFactor};
    }
    std::vector<RHINrdBackend::PoolTexture> transient(desc.transientPoolSize);
    for (uint32_t i = 0; i < desc.transientPoolSize; i++)
    {
        transient[i] = {.format = static_cast<uint32_t>(desc.transientPool[i].format),
                        .downsample_factor = desc.transientPool[i].downsampleFactor};
    }

    backend_->AllocateResources(permanent.data(), desc.permanentPoolSize, transient.data(), desc.transientPoolSize,
                                reinterpret_cast<const uint32_t *>(desc.samplers), desc.samplersNum,
                                desc.constantBufferMaxDataSize);
    reblur_pass_ = rhi_->CreateComputePass("NrdReblurPass", true);

    pack_ubo_ = rhi_->CreateBuffer({.size = sizeof(NrdPackShader::UniformBufferData),
                                    .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                    .mem_properties = RHIMemoryProperty::None,
                                    .is_dynamic = true},
                                   "NrdPackUBO");
    pack_shader_ = rhi_->CreateShader<NrdPackShader>();
    pack_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "NrdPackPipeline");
    pack_pipeline_->SetShader<RHIShaderStage::Compute>(pack_shader_);
    pack_pipeline_->Compile();
    pack_pipeline_->GetShaderResource<NrdPackShader>()->ubo().BindResource(pack_ubo_);
    pack_pass_ = rhi_->CreateComputePass("NrdPackPass", true);

    resolve_ubo_ = rhi_->CreateBuffer({.size = sizeof(NrdResolveShader::UniformBufferData),
                                       .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                       .mem_properties = RHIMemoryProperty::None,
                                       .is_dynamic = true},
                                      "NrdResolveUBO");
    resolve_shader_ = rhi_->CreateShader<NrdResolveShader>();
    resolve_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "NrdResolvePipeline");
    resolve_pipeline_->SetShader<RHIShaderStage::Compute>(resolve_shader_);
    resolve_pipeline_->Compile();
    resolve_pipeline_->GetShaderResource<NrdResolveShader>()->ubo().BindResource(resolve_ubo_);
    resolve_pass_ = rhi_->CreateComputePass("NrdResolvePass", true);

    CreateTextures(output_format);

    timings_.AddStage("pack", pack_pass_);
    timings_.AddStage("reblur", reblur_pass_);
    timings_.AddStage("resolve", resolve_pass_);

    enabled_resources_ready_ = true;
}

void NrdDenoiser::CreateTextures(PixelFormat output_format)
{
    backend_->ResizePools(input_size_.x(), input_size_.y());

    // half precision per NRD's own format recommendations (radiance/MV); normal+roughness matches
    // NRD_NORMAL_ENCODING=2 (oct-packed R10G10B10A2); viewZ stays a full 32-bit float (plane-distance
    // disocclusion precision).
    in_mv_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInMv");
    in_normal_roughness_ = CreateFullScreenTexture(PixelFormat::R10G10B10A2Unorm, "NrdInNormalRoughness");
    in_viewz_ = CreateFullScreenTexture(PixelFormat::R32Float, "NrdInViewZ");
    in_diff_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInDiff");
    in_spec_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInSpec");
    out_diff_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdOutDiff");
    out_spec_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdOutSpec");
    validation_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdValidation");

    // the accumulator's precision, so the final resolve can equal the accumulator bit-exactly
    output_ = CreateFullScreenTexture(output_format, "NrdOutput");
    output_history_ = CreateFullScreenTexture(output_format, "NrdOutputHistory");

    auto *pack = pack_pipeline_->GetShaderResource<NrdPackShader>();
    pack->outMv().BindResource(in_mv_->GetDefaultView(rhi_));
    pack->outNormalRoughness().BindResource(in_normal_roughness_->GetDefaultView(rhi_));
    pack->outViewZ().BindResource(in_viewz_->GetDefaultView(rhi_));
    pack->outDiff().BindResource(in_diff_->GetDefaultView(rhi_));
    pack->outSpec().BindResource(in_spec_->GetDefaultView(rhi_));

    auto *resolve = resolve_pipeline_->GetShaderResource<NrdResolveShader>();
    resolve->denoisedDiff().BindResource(out_diff_->GetDefaultView(rhi_));
    resolve->denoisedSpec().BindResource(out_spec_->GetDefaultView(rhi_));
    resolve->inMv().BindResource(in_mv_->GetDefaultView(rhi_));
    resolve->inNormalRoughness().BindResource(in_normal_roughness_->GetDefaultView(rhi_));
    resolve->inViewZ().BindResource(in_viewz_->GetDefaultView(rhi_));
    resolve->inDiff().BindResource(in_diff_->GetDefaultView(rhi_));
    resolve->inSpec().BindResource(in_spec_->GetDefaultView(rhi_));
    resolve->validation().BindResource(validation_->GetDefaultView(rhi_));
}

void NrdDenoiser::Resize(const Vector2UInt &input_size, const Vector2UInt & /*output_size*/)
{
    input_size_ = input_size;
    if (enabled_resources_ready_)
    {
        CreateTextures(output_->GetAttributes().format);
        reset_history_ = true;
    }
}

void NrdDenoiser::BindInputs(const RGPassContext &context, const DenoiserInputs &inputs)
{
    const auto view = [this, &context](RGTexture texture) { return context.GetImage(texture)->GetDefaultView(rhi_); };

    auto *pack_resources = pack_pipeline_->GetShaderResource<NrdPackShader>();
    pack_resources->gRadiance().BindResource(view(inputs.noisy_radiance_hit_distance));
    pack_resources->gRadianceSpecular().BindResource(view(inputs.noisy_specular_radiance_hit_distance));
    pack_resources->gNormalDepth().BindResource(view(inputs.normal_view_depth));
    pack_resources->gAlbedoObj().BindResource(view(inputs.albedo_object_id));
    pack_resources->gMotion().BindResource(view(inputs.motion_hit_metallic));
    pack_resources->gSpecAlbedo().BindResource(view(inputs.specular_albedo_roughness));

    auto *resolve_resources = resolve_pipeline_->GetShaderResource<NrdResolveShader>();
    resolve_resources->gRadiance().BindResource(view(inputs.noisy_radiance_hit_distance));
    resolve_resources->gAlbedoObj().BindResource(view(inputs.albedo_object_id));
    resolve_resources->gMotion().BindResource(view(inputs.motion_hit_metallic));
    resolve_resources->gSpecAlbedo().BindResource(view(inputs.specular_albedo_roughness));
    resolve_resources->outputImage().BindResource(output_->GetDefaultView(rhi_));
    resolve_resources->sceneAccum().BindResource(view(inputs.accumulated_radiance));
    resolve_resources->outputHistory().BindResource(output_history_->GetDefaultView(rhi_));
}

void NrdDenoiser::UpdateFrameData(const DenoiserFrameData &frame)
{
    SampleConfig();
    far_plane_ = frame.far_plane;
    view_matrix_ = frame.view;
    projection_matrix_ = frame.projection;
    cumulated_samples_ = frame.accumulated_samples;
    max_sample_per_pixel_ = frame.maximum_samples;
    reset_history_ = reset_history_ || frame.reset_history;

    // once fully handed off to the accumulator, ReBLUR is skipped and the resolve ignores the
    // G-buffer, so the path tracer can stop writing it; debug views keep it live
    const DenoiserHandoff handoff(max_sample_per_pixel_);
    needs_inputs_ = config_.debug_mode != NrdDebugMode::None || !handoff.Applies() ||
                    static_cast<float>(cumulated_samples_) < handoff.GetEnd();
}

// NRD's pool, IN_* and OUT_* textures stay private to the pass and keep their own transitions
RGTexture NrdDenoiser::AddTo(RenderGraph &graph, const DenoiserInputs &inputs)
{
    ASSERT(enabled_resources_ready_);

    const auto output = graph.Import("NrdOutput", output_);
    const auto output_history = graph.Import("NrdOutputHistory", output_history_);
    graph.AddExternalPass("Nrd", [this, inputs, output, output_history](RGBuilder &builder) {
        for (const auto input : {inputs.noisy_radiance_hit_distance, inputs.normal_view_depth, inputs.albedo_object_id,
                                 inputs.motion_hit_metallic, inputs.noisy_specular_radiance_hit_distance,
                                 inputs.specular_albedo_roughness, inputs.accumulated_radiance})
        {
            builder.Sampled(input, RHIShaderStageMask::Compute);
        }
        builder.StorageWrite(output, RHIShaderStageMask::Compute);
        builder.StorageReadWrite(output_history, RHIShaderStageMask::Compute);
        return [this, inputs](RGExternalContext &context) { Encode(context, inputs); };
    });
    return output;
}

void NrdDenoiser::Encode(RGExternalContext &context, const DenoiserInputs &inputs)
{
    auto &command_context = context.GetCommandContext();

    BindInputs(context, inputs);

    const auto &attr = output_->GetAttributes();
    const Vector3UInt dispatch{attr.width, attr.height, 1u};
    const Vector3UInt group{16u, 16u, 1u};

    const float samples_this_frame =
        static_cast<float>(std::max<int64_t>(static_cast<int64_t>(cumulated_samples_) - last_cumulated_samples_, 1));
    last_cumulated_samples_ = cumulated_samples_;

    // cumulated_samples_ is the pre-dispatch count, but the resolve reads the accumulator AFTER this
    // frame's dispatch, so the handoff weight is computed from the post-dispatch count. When this frame
    // completes the target (the render freezes after it), the resolve is the last one to ever run: pin
    // the weight to 1 (and beta to 0 below) so the frozen frame IS the accumulator, bit-exact.
    const float post_samples = static_cast<float>(cumulated_samples_) + samples_this_frame;
    const DenoiserHandoff handoff(max_sample_per_pixel_);
    const bool final_resolve = post_samples >= static_cast<float>(max_sample_per_pixel_);
    const float handoff_weight = handoff.ComputeWeight(post_samples, final_resolve);

    // The EMA must track the composite's own convergence: the signal changes by ~spp/N per frame (new
    // samples' weight in the accumulating mean), so the EMA attenuation is set to 3x that rate — fast
    // enough to never lag the convergence (at any spp, incl. dynamic), slow enough that an isolated pop
    // still displays at only ~3*spp/N of its amplitude. Under motion N resets to ~spp, driving beta to 0.
    const float stabilization_rate = 3.f * samples_this_frame / static_cast<float>(std::max(cumulated_samples_, 1u));

    // The EMA must also die out with the ReBLUR contribution it smooths: its lag otherwise keeps the
    // display on an older, more-ReBLUR-flavored mix than handoff_weight says, and the final resolve
    // (beta pinned to 0) would release that lag as a visible pop right at the freeze.
    const float stabilization_beta = std::clamp(1.f - stabilization_rate, 0.f, 0.99f) * (1.f - handoff_weight);

    // fully handed off to the accumulator: the ReBLUR result is weighted by zero, so skip its ~1.7 ms of
    // dispatches and run only the resolve (composite + stabilization). ReBLUR's history stays valid — the
    // camera is static for as long as this branch holds, and any motion resets cumulated_samples_ -> w < 1.
    const bool run_reblur = handoff_weight < 1.f || config_.debug_mode != NrdDebugMode::None;

    // pack and reblur are dispatched together, so they share one decision; resolve always runs
    timings_.Sample({run_reblur, run_reblur, true});

    if (reset_history_)
    {
        prev_view_matrix_ = view_matrix_;
        prev_projection_matrix_ = projection_matrix_;
    }

    if (run_reblur)
    {
        RenderReblur(command_context, dispatch, group);
    }

    // every sampled private texture, not just the ReBLUR outputs: on handoff frames the ReBLUR block (and its
    // layout epilogues) is skipped entirely while the resolve still binds in_*
    for (const auto &image :
         {out_diff_, out_spec_, validation_, in_mv_, in_normal_roughness_, in_viewz_, in_diff_, in_spec_})
    {
        ToLayout(command_context, image, RHIImageLayout::Read, RHIPipelineStage::ComputeShader,
                 RHIPipelineStage::ComputeShader);
    }

    NrdResolveShader::UniformBufferData resolve_ubo{
        .resolution = Vector2UInt(attr.width, attr.height),
        .mode = static_cast<uint32_t>(config_.debug_mode),
        .demod_eps = DemodEps,
        .view_z_scale = far_plane_ * 0.05f,
        .motion_scale = MotionDisplayScale,
        .handoff_weight = handoff_weight,
        .stabilization_beta = (reset_history_ || final_resolve) ? 0.f : stabilization_beta,
    };
    resolve_ubo_->Upload(rhi_, &resolve_ubo);

    command_context.BeginComputePass(resolve_pass_);
    command_context.DispatchCompute(resolve_pipeline_, dispatch, group);
    command_context.EndComputePass(resolve_pass_);

    prev_view_matrix_ = view_matrix_;
    prev_projection_matrix_ = projection_matrix_;
    reset_history_ = false;
}

void NrdDenoiser::RenderReblur(RHICommandContext &command_context, const Vector3UInt &dispatch,
                               const Vector3UInt &group)
{
    const auto &attr = output_->GetAttributes();

    for (const auto &image : {in_mv_, in_normal_roughness_, in_viewz_, in_diff_, in_spec_})
    {
        ToLayout(command_context, image, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader,
                 RHIPipelineStage::ComputeShader);
    }

    NrdPackShader::UniformBufferData pack_ubo{
        .resolution = Vector2UInt(attr.width, attr.height),
        .hit_dist_a = HitDistA,
        .hit_dist_b = HitDistB,
        .hit_dist_c = HitDistC,
        .demod_eps = DemodEps,
        .sky_view_z = SkyViewZ,
    };
    pack_ubo_->Upload(rhi_, &pack_ubo);

    command_context.BeginComputePass(pack_pass_);
    command_context.DispatchCompute(pack_pipeline_, dispatch, group);
    command_context.EndComputePass(pack_pass_);

    // ReBLUR reads the freshly packed inputs and writes the OUT_* textures on its own encoder.
    for (const auto &image : {in_mv_, in_normal_roughness_, in_viewz_, in_diff_, in_spec_})
    {
        ToLayout(command_context, image, RHIImageLayout::Read, RHIPipelineStage::ComputeShader,
                 RHIPipelineStage::ComputeShader);
    }
    for (const auto &image : {out_diff_, out_spec_})
    {
        ToLayout(command_context, image, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader,
                 RHIPipelineStage::ComputeShader);
    }

    // NRD assumes D3D clip conventions (+Y up); undo the engine's Vulkan-style Y flip (proj(1,1) < 0)
    // or all matrix-derived reprojection is mirrored vs IN_MV (breaks pitch/roll/vertical motion).
    Mat4 view_to_clip = projection_matrix_;
    view_to_clip.row(1) *= -1.f;
    Mat4 view_to_clip_prev = prev_projection_matrix_;
    view_to_clip_prev.row(1) *= -1.f;

    nrd::CommonSettings cs{};
    CopyMatrix(cs.viewToClipMatrix, view_to_clip);
    CopyMatrix(cs.viewToClipMatrixPrev, view_to_clip_prev);
    CopyMatrix(cs.worldToViewMatrix, view_matrix_);
    CopyMatrix(cs.worldToViewMatrixPrev, prev_view_matrix_);
    cs.motionVectorScale[0] = 1.f;
    cs.motionVectorScale[1] = 1.f;
    cs.motionVectorScale[2] = 0.f;
    cs.resourceSize[0] = static_cast<uint16_t>(attr.width);
    cs.resourceSize[1] = static_cast<uint16_t>(attr.height);
    cs.resourceSizePrev[0] = cs.resourceSize[0];
    cs.resourceSizePrev[1] = cs.resourceSize[1];
    cs.rectSize[0] = cs.resourceSize[0];
    cs.rectSize[1] = cs.resourceSize[1];
    cs.rectSizePrev[0] = cs.resourceSize[0];
    cs.rectSizePrev[1] = cs.resourceSize[1];
    cs.denoisingRange = far_plane_ * 2.f;
    cs.frameIndex = frame_index_++;
    cs.accumulationMode = reset_history_ ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    cs.isMotionVectorInWorldSpace = false;
    cs.enableValidation = config_.debug_mode == NrdDebugMode::Validation;
    if (nrd::SetCommonSettings(*instance_, cs) != nrd::Result::SUCCESS)
    {
        Log(Error, "NRD: SetCommonSettings failed");
        return;
    }

    nrd::ReblurSettings rs{};
    rs.hitDistanceParameters.A = HitDistA;
    rs.hitDistanceParameters.B = HitDistB;
    rs.hitDistanceParameters.C = HitDistC;
    rs.enableAntiFirefly = true;
    rs.maxBlurRadius = 15.f;
    // the pack marks skipped-lobe frames with hitT = 0 (probabilistic primary-lobe selection); NRD requires
    // reconstruction to fill those from neighbors
    rs.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
    // the test is a raw float equality between two differently-derived unorm decodes; it only holds
    // on every driver because the pack writes IDs 0/3, the exact unorm endpoints (see nrd_pack)
    rs.minMaterialForDiffuse = 0.f;
    rs.minMaterialForSpecular = 0.f;
    if (!config_.stabilization)
    {
        rs.maxStabilizedFrameNum = 0;
    }
    nrd::SetDenoiserSettings(*instance_, 0, &rs);

    const nrd::DispatchDesc *dispatches = nullptr;
    uint32_t dispatch_count = 0;
    const nrd::Identifier identifier = 0;
    if (nrd::GetComputeDispatches(*instance_, &identifier, 1, dispatches, dispatch_count) != nrd::Result::SUCCESS)
    {
        Log(Error, "NRD: GetComputeDispatches failed");
        return;
    }

    seam_dispatches_.resize(dispatch_count);
    size_t total_resources = 0;
    for (uint32_t d = 0; d < dispatch_count; d++)
    {
        total_resources += dispatches[d].resourcesNum;
    }
    seam_resources_.resize(total_resources);

    size_t resource_cursor = 0;
    for (uint32_t d = 0; d < dispatch_count; d++)
    {
        const nrd::DispatchDesc &src = dispatches[d];
        RHINrdBackend::DispatchResource *resources = seam_resources_.data() + resource_cursor;
        resource_cursor += src.resourcesNum;
        for (uint32_t r = 0; r < src.resourcesNum; r++)
        {
            const nrd::ResourceDesc &res = src.resources[r];
            auto &out = resources[r];
            out.is_uav = res.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            out.index_in_pool = res.indexInPool;
            out.user_image = nullptr;
            switch (res.type)
            {
            case nrd::ResourceType::PERMANENT_POOL:
                out.source = RHINrdBackend::DispatchResource::Source::PermanentPool;
                break;
            case nrd::ResourceType::TRANSIENT_POOL:
                out.source = RHINrdBackend::DispatchResource::Source::TransientPool;
                break;
            default: {
                out.source = RHINrdBackend::DispatchResource::Source::User;
                switch (res.type)
                {
                case nrd::ResourceType::IN_MV:
                    out.user_image = in_mv_.get();
                    break;
                case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                    out.user_image = in_normal_roughness_.get();
                    break;
                case nrd::ResourceType::IN_VIEWZ:
                    out.user_image = in_viewz_.get();
                    break;
                case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                    out.user_image = in_diff_.get();
                    break;
                case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                    out.user_image = in_spec_.get();
                    break;
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                    out.user_image = out_diff_.get();
                    break;
                case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                    out.user_image = out_spec_.get();
                    break;
                case nrd::ResourceType::OUT_VALIDATION:
                    out.user_image = validation_.get();
                    break;
                default:
                    ASSERT_F(false, "NRD: unhandled resource type {} in dispatch '{}'", static_cast<uint32_t>(res.type),
                             src.name ? src.name : "?");
                }
                break;
            }
            }
        }

        seam_dispatches_[d] = {
            .pipeline_index = src.pipelineIndex,
            .grid_width = src.gridWidth,
            .grid_height = src.gridHeight,
            .constant_data = src.constantBufferData,
            .constant_size = src.constantBufferDataSize,
            .resources = resources,
            .resource_count = src.resourcesNum,
        };
    }

    command_context.BeginComputePass(reblur_pass_);
    backend_->RunDispatches(command_context, seam_dispatches_.data(), dispatch_count);
    command_context.EndComputePass(reblur_pass_);
}
} // namespace sparkle
