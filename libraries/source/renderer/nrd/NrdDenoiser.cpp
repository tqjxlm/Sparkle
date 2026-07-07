#include "renderer/nrd/NrdDenoiser.h"

#include "renderer/nrd/NrdCookedShaders.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

#include <NRD.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace sparkle
{
namespace
{
constexpr float DemodEps = 1e-3f;
constexpr float SkyViewZ = 1e6f;
constexpr float MotionDisplayScale = 20.f;

// Convergence handoff (ReBLUR's capped history re-blends fresh 1-spp noise forever -> permanent shimmer
// on a static view): fade the NRD composite into the progressive accumulator as it converges. Start
// after the accumulator's own grain drops below NRD's noise floor; fully converged by End.
// Late handoff: the output-stabilization EMA (below) already suppresses ReBLUR's pop/churn in the low-N
// window, so showing the raw accumulator early only exposes its grain (5-12% at N<400, the "dirty" look).
// Hand over once the accumulator's grain is near-invisible (~2% at N=2048); convergence to the exact
// accumulated image is preserved, just later.
constexpr float HandoffStartSamples = 512.f;
constexpr float HandoffEndSamples = 2048.f;

// must match the ReblurHitDistanceParameters fed to NRD (SetDenoiserSettings) — the pack shader
// normalizes hit distances with the same constants ReBLUR denormalizes with.
constexpr float HitDistA = 3.0f;
constexpr float HitDistB = 0.1f;
constexpr float HitDistC = 20.0f;

void ToLayout(const RHIResourceRef<RHIImage> &image, RHIImageLayout layout, RHIPipelineStage after,
              RHIPipelineStage before)
{
    image->Transition({.target_layout = layout, .after_stage = after, .before_stage = before});
}

void CopyMatrix(float (&dst)[16], const Mat4 &src)
{
    std::memcpy(dst, src.data(), sizeof(dst));
}
} // namespace

class NrdPackShader : public RHIShaderInfo
{
    REGISTGER_SHADER(NrdPackShader, RHIShaderStage::Compute, "shaders/nrd/nrd_pack.cs.slang", "main")

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
    REGISTGER_SHADER(NrdResolveShader, RHIShaderStage::Compute, "shaders/nrd/nrd_resolve.cs.slang", "main")

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

NrdDenoiser::NrdDenoiser(RHIContext *ctx, RHIResourceRef<RHIImage> input)
    : PipelinePass(ctx), input_(std::move(input))
{
}

NrdDenoiser::~NrdDenoiser()
{
    if (instance_)
    {
        nrd::DestroyInstance(*instance_);
    }
}

RHIResourceRef<RHIImage> NrdDenoiser::CreateFullScreenTexture(PixelFormat format, const std::string &name)
{
    const auto &input_attr = input_->GetAttributes();

    auto image = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = format,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = input_attr.width,
            .height = input_attr.height,
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        name);

    ToLayout(image, RHIImageLayout::Read, RHIPipelineStage::Top, RHIPipelineStage::ComputeShader);

    return image;
}

void NrdDenoiser::InitRenderResources(const RenderConfig &)
{
    if (config_.enabled)
    {
        EnsureEnabledResources();
        return;
    }

    // Disabled: the path-tracer pipeline still declares the G-buffer bindings, so point them at 1x1
    // dummies (the shader never writes them while write_gbuffer is 0) instead of paying six full-res
    // float targets. EnsureEnabledResources replaces them with real targets on first enable.
    auto dummy = [this](PixelFormat format) {
        return rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
            .format = format,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        });
    };
    g_radiance_ = dummy(PixelFormat::RGBAFloat);
    g_normal_depth_ = dummy(PixelFormat::RGBAFloat);
    g_albedo_obj_ = dummy(PixelFormat::RGBAFloat);
    g_radiance_specular_ = dummy(PixelFormat::RGBAFloat);
    g_motion_ = dummy(PixelFormat::RGBAFloat16);
    g_spec_albedo_ = dummy(PixelFormat::RGBAFloat16);
}

void NrdDenoiser::AllocateGBuffer()
{
    g_radiance_ = CreateFullScreenTexture(PixelFormat::RGBAFloat, "GBufferRadiance");
    g_normal_depth_ = CreateFullScreenTexture(PixelFormat::RGBAFloat, "GBufferNormalDepth");
    g_albedo_obj_ = CreateFullScreenTexture(PixelFormat::RGBAFloat, "GBufferAlbedoObj");
    g_radiance_specular_ = CreateFullScreenTexture(PixelFormat::RGBAFloat, "GBufferRadianceSpecular");
    g_motion_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "GBufferMotion");
    g_spec_albedo_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "GBufferSpecAlbedo");
}

void NrdDenoiser::BeginGBufferWrite()
{
    for (const auto &image : {g_radiance_, g_normal_depth_, g_albedo_obj_, g_motion_, g_radiance_specular_,
                              g_spec_albedo_})
    {
        ToLayout(image, RHIImageLayout::StorageWrite, RHIPipelineStage::Top, RHIPipelineStage::ComputeShader);
    }
}

void NrdDenoiser::EnsureEnabledResources()
{
    // failures latch permanently (IsActive() -> false): they are deterministic, and retrying every
    // frame would re-run the full SPIRV->MSL pipeline compilation and leak nrd instances
    if (enabled_resources_ready_ || enabled_resources_failed_)
    {
        return;
    }

    AllocateGBuffer();

    backend_ = rhi_->CreateNrdBackend();
    if (!backend_)
    {
        Log(Error, "NRD: this RHI has no NRD backend (Metal only); denoiser stays disabled");
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
    if (!LoadNrdCookedShaders(cooked))
    {
        enabled_resources_failed_ = true;
        return;
    }
    if (cooked.version_major != lib.versionMajor || cooked.version_minor != lib.versionMinor ||
        cooked.version_build != lib.versionBuild || cooked.pipelines.size() != desc.pipelinesNum)
    {
        Log(Error,
            "NRD: cooked shaders are stale (cooked {}.{}.{}, {} pipelines; NRD {}.{}.{}, {} pipelines). "
            "run dev/cook_nrd_shaders.py",
            cooked.version_major, cooked.version_minor, cooked.version_build, cooked.pipelines.size(),
            lib.versionMajor, lib.versionMinor, lib.versionBuild, desc.pipelinesNum);
        enabled_resources_failed_ = true;
        return;
    }

    uint32_t ok = 0;
    for (uint32_t i = 0; i < desc.pipelinesNum; i++)
    {
        if (cooked.identifiers[i] != desc.pipelines[i].shaderIdentifier)
        {
            Log(Error, "NRD: cooked pipeline [{}] is '{}', NRD expects '{}'. run dev/cook_nrd_shaders.py", i,
                cooked.identifiers[i], desc.pipelines[i].shaderIdentifier);
            break;
        }
        if (backend_->AddPipeline(cooked.pipelines[i]))
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

    const auto &input_attr = input_->GetAttributes();
    backend_->AllocateResources(input_attr.width, input_attr.height, permanent.data(), desc.permanentPoolSize,
                                transient.data(), desc.transientPoolSize,
                                reinterpret_cast<const uint32_t *>(desc.samplers), desc.samplersNum,
                                desc.constantBufferMaxDataSize);

    // half precision per NRD's own format recommendations (radiance/normals/MV); viewZ stays a full
    // 32-bit float (plane-distance disocclusion precision). output_ and output_history_ must keep the
    // accumulator's format: the final resolve copies it through bit-exact.
    in_mv_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInMv");
    in_normal_roughness_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInNormalRoughness");
    in_viewz_ = CreateFullScreenTexture(PixelFormat::R32_FLOAT, "NrdInViewZ");
    in_diff_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInDiff");
    in_spec_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdInSpec");
    out_diff_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdOutDiff");
    out_spec_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdOutSpec");
    validation_ = CreateFullScreenTexture(PixelFormat::RGBAFloat16, "NrdValidation");
    output_ = CreateFullScreenTexture(input_attr.format, "NrdOutput");
    output_history_ = CreateFullScreenTexture(input_attr.format, "NrdOutputHistory");

    pack_ubo_ = rhi_->CreateBuffer({.size = sizeof(NrdPackShader::UniformBufferData),
                                    .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                    .mem_properties = RHIMemoryProperty::None,
                                    .is_dynamic = true},
                                   "NrdPackUBO");
    pack_shader_ = rhi_->CreateShader<NrdPackShader>();
    pack_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "NrdPackPipeline");
    pack_pipeline_->SetShader<RHIShaderStage::Compute>(pack_shader_);
    pack_pipeline_->Compile();
    {
        auto *r = pack_pipeline_->GetShaderResource<NrdPackShader>();
        r->ubo().BindResource(pack_ubo_);
        r->gRadiance().BindResource(g_radiance_->GetDefaultView(rhi_));
        r->gRadianceSpecular().BindResource(g_radiance_specular_->GetDefaultView(rhi_));
        r->gNormalDepth().BindResource(g_normal_depth_->GetDefaultView(rhi_));
        r->gAlbedoObj().BindResource(g_albedo_obj_->GetDefaultView(rhi_));
        r->gMotion().BindResource(g_motion_->GetDefaultView(rhi_));
        r->gSpecAlbedo().BindResource(g_spec_albedo_->GetDefaultView(rhi_));
        r->outMv().BindResource(in_mv_->GetDefaultView(rhi_));
        r->outNormalRoughness().BindResource(in_normal_roughness_->GetDefaultView(rhi_));
        r->outViewZ().BindResource(in_viewz_->GetDefaultView(rhi_));
        r->outDiff().BindResource(in_diff_->GetDefaultView(rhi_));
        r->outSpec().BindResource(in_spec_->GetDefaultView(rhi_));
    }
    pack_pass_ = rhi_->CreateComputePass("NrdPackPass", false);

    resolve_ubo_ = rhi_->CreateBuffer({.size = sizeof(NrdResolveShader::UniformBufferData),
                                       .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                       .mem_properties = RHIMemoryProperty::None,
                                       .is_dynamic = true},
                                      "NrdResolveUBO");
    resolve_shader_ = rhi_->CreateShader<NrdResolveShader>();
    resolve_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "NrdResolvePipeline");
    resolve_pipeline_->SetShader<RHIShaderStage::Compute>(resolve_shader_);
    resolve_pipeline_->Compile();
    {
        auto *r = resolve_pipeline_->GetShaderResource<NrdResolveShader>();
        r->ubo().BindResource(resolve_ubo_);
        r->denoisedDiff().BindResource(out_diff_->GetDefaultView(rhi_));
        r->denoisedSpec().BindResource(out_spec_->GetDefaultView(rhi_));
        r->inMv().BindResource(in_mv_->GetDefaultView(rhi_));
        r->inNormalRoughness().BindResource(in_normal_roughness_->GetDefaultView(rhi_));
        r->inViewZ().BindResource(in_viewz_->GetDefaultView(rhi_));
        r->inDiff().BindResource(in_diff_->GetDefaultView(rhi_));
        r->inSpec().BindResource(in_spec_->GetDefaultView(rhi_));
        r->gRadiance().BindResource(g_radiance_->GetDefaultView(rhi_));
        r->gAlbedoObj().BindResource(g_albedo_obj_->GetDefaultView(rhi_));
        r->gMotion().BindResource(g_motion_->GetDefaultView(rhi_));
        r->gSpecAlbedo().BindResource(g_spec_albedo_->GetDefaultView(rhi_));
        r->outputImage().BindResource(output_->GetDefaultView(rhi_));
        r->validation().BindResource(validation_->GetDefaultView(rhi_));
        r->sceneAccum().BindResource(input_->GetDefaultView(rhi_));
        r->outputHistory().BindResource(output_history_->GetDefaultView(rhi_));
    }
    resolve_pass_ = rhi_->CreateComputePass("NrdResolvePass", false);

    enabled_resources_ready_ = true;
}

NrdDenoiser::HandoffWindow NrdDenoiser::ComputeHandoffWindow() const
{
    // max_spp below the window opts out: the motion harnesses (max_spp=1) rely on the frozen frame
    // being the last ReBLUR output, not the raw low-spp accumulator
    const bool applies = static_cast<float>(max_sample_per_pixel_) >= HandoffStartSamples;
    const float end =
        applies ? std::min(HandoffEndSamples, static_cast<float>(max_sample_per_pixel_)) : HandoffEndSamples;
    return {.applies = applies, .start = std::min(HandoffStartSamples, 0.25f * end), .end = end};
}

void NrdDenoiser::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    auto *camera = scene->GetCamera();
    far_plane_ = camera->GetFar();
    view_matrix_ = camera->GetViewMatrix();
    projection_matrix_ = camera->GetProjectionMatrix();
    cumulated_samples_ = camera->GetCumulatedSampleCount();
    max_sample_per_pixel_ = config.max_sample_per_pixel;

    // once fully handed off to the accumulator, ReBLUR is skipped and the resolve ignores the
    // G-buffer, so the path tracer can stop writing it; debug views keep it live
    const HandoffWindow window = ComputeHandoffWindow();
    needs_gbuffer_ = config_.debug_mode != NrdDebugMode::None || !window.applies ||
                     static_cast<float>(cumulated_samples_) < window.end;
}

void NrdDenoiser::Render()
{
    if (!enabled_resources_ready_)
    {
        return;
    }

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
    const HandoffWindow window = ComputeHandoffWindow();
    const bool final_resolve = window.applies && post_samples >= static_cast<float>(max_sample_per_pixel_);
    const float handoff_weight =
        final_resolve ? 1.f : std::clamp((post_samples - window.start) / (window.end - window.start), 0.f, 1.f);

    // The EMA must track the composite's own convergence: the signal changes by ~spp/N per frame (new
    // samples' weight in the accumulating mean), so the EMA attenuation is set to 3x that rate — fast
    // enough to never lag the convergence (at any spp, incl. dynamic), slow enough that an isolated pop
    // still displays at only ~3*spp/N of its amplitude. Under motion N resets to ~spp, driving beta to 0.
    const float stabilization_rate = 3.f * samples_this_frame / static_cast<float>(std::max(cumulated_samples_, 1u));

    // fully handed off to the accumulator: the ReBLUR result is weighted by zero, so skip its ~1.7 ms of
    // dispatches and run only the resolve (composite + stabilization). ReBLUR's history stays valid — the
    // camera is static for as long as this branch holds, and any motion resets cumulated_samples_ -> w < 1.
    const bool run_reblur = handoff_weight < 1.f || config_.debug_mode != NrdDebugMode::None;

    if (reset_history_)
    {
        prev_view_matrix_ = view_matrix_;
        prev_projection_matrix_ = projection_matrix_;
    }

    if (run_reblur)
    {
        RenderReblur(dispatch, group);
    }

    for (const auto &image : {out_diff_, out_spec_, validation_})
    {
        ToLayout(image, RHIImageLayout::Read, RHIPipelineStage::ComputeShader, RHIPipelineStage::ComputeShader);
    }
    ToLayout(output_, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader, RHIPipelineStage::ComputeShader);
    ToLayout(output_history_, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader,
             RHIPipelineStage::ComputeShader);

    NrdResolveShader::UniformBufferData resolve_ubo{
        .resolution = Vector2UInt(attr.width, attr.height),
        .mode = static_cast<uint32_t>(config_.debug_mode),
        .demod_eps = DemodEps,
        .view_z_scale = far_plane_ * 0.05f,
        .motion_scale = MotionDisplayScale,
        .handoff_weight = handoff_weight,
        .stabilization_beta =
            (reset_history_ || final_resolve) ? 0.f : std::clamp(1.f - stabilization_rate, 0.f, 0.99f),
    };
    resolve_ubo_->Upload(rhi_, &resolve_ubo);

    rhi_->BeginComputePass(resolve_pass_);
    rhi_->DispatchCompute(resolve_pipeline_, dispatch, group);
    rhi_->EndComputePass(resolve_pass_);

    ToLayout(output_, RHIImageLayout::Read, RHIPipelineStage::ComputeShader, RHIPipelineStage::PixelShader);

    prev_view_matrix_ = view_matrix_;
    prev_projection_matrix_ = projection_matrix_;
    reset_history_ = false;
}

void NrdDenoiser::RenderReblur(const Vector3UInt &dispatch, const Vector3UInt &group)
{
    const auto &attr = output_->GetAttributes();

    for (const auto &image : {g_radiance_, g_radiance_specular_, g_normal_depth_, g_albedo_obj_, g_motion_,
                              g_spec_albedo_})
    {
        ToLayout(image, RHIImageLayout::Read, RHIPipelineStage::ComputeShader, RHIPipelineStage::ComputeShader);
    }
    for (const auto &image : {in_mv_, in_normal_roughness_, in_viewz_, in_diff_, in_spec_})
    {
        ToLayout(image, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader,
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

    rhi_->BeginComputePass(pack_pass_);
    rhi_->DispatchCompute(pack_pipeline_, dispatch, group);
    rhi_->EndComputePass(pack_pass_);

    // ReBLUR reads the freshly packed inputs and writes the OUT_* textures on its own encoder.
    for (const auto &image : {in_mv_, in_normal_roughness_, in_viewz_, in_diff_, in_spec_})
    {
        ToLayout(image, RHIImageLayout::Read, RHIPipelineStage::ComputeShader, RHIPipelineStage::ComputeShader);
    }
    for (const auto &image : {out_diff_, out_spec_})
    {
        ToLayout(image, RHIImageLayout::StorageWrite, RHIPipelineStage::ComputeShader,
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
                    ASSERT_F(false, "NRD: unhandled resource type {} in dispatch '{}'",
                             static_cast<uint32_t>(res.type), src.name ? src.name : "?");
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

    backend_->RunDispatches(seam_dispatches_.data(), dispatch_count);
}
} // namespace sparkle
