#if FRAMEWORK_APPLE

#include "MetalFxDenoiser.h"

#include "MetalContext.h"
#include "MetalImage.h"
#include "MetalRHIInternal.h"
#include "core/Logger.h"
#include "renderer/denoiser/DenoiserFactory.h"
#include "renderer/denoiser/DenoiserHandoff.h"
#include "renderer/denoiser/PassTimingAggregator.h"
#include "rhi/RHI.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIShader.h"

#if !defined(SPARKLE_DISABLE_METALFX) && __has_include(<MetalFX/MTLFXTemporalDenoisedScaler.h>)
#import <MetalFX/MetalFX.h>
#define SPARKLE_HAS_METALFX_DENOISED 1
#else
#define SPARKLE_HAS_METALFX_DENOISED 0
#endif

#include <algorithm>

namespace sparkle
{
namespace
{
constexpr MTLTextureUsage KnownTextureUsages = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                                               MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView |
                                               MTLTextureUsageShaderAtomic;

#if SPARKLE_HAS_METALFX_DENOISED
simd_float4x4 ToSimdMatrix(const Mat4 &source)
{
    simd_float4x4 result{};
    for (int column = 0; column < 4; column++)
    {
        for (int row = 0; row < 4; row++)
        {
            result.columns[column][row] = source(row, column);
        }
    }
    return result;
}
#endif

bool HasOnlyKnownUsages(MTLTextureUsage usage, const char *name)
{
    const auto unsupported = usage & ~KnownTextureUsages;
    if (unsupported == 0)
    {
        return true;
    }

    Log(Error, "MetalFX: {} requests unsupported texture usage bits 0x{:x}", name, static_cast<uint64_t>(unsupported));
    return false;
}
} // namespace

class MetalFxPrepareShader : public RHIShaderInfo
{
    REGISTGER_SHADER(MetalFxPrepareShader, RHIShaderStage::Compute, "shaders/metalfx/metalfx_prepare.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(sceneRadiance, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(normalViewDepth, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(albedoObjectId, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(motionHitMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(specularAlbedoRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outColor, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outDepth, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outMotion, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outDiffuseAlbedo, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecularAlbedo, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outNormal, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outRoughness, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Mat4 projection;
        Vector2UInt resolution;
        float far_depth;
        float padding = 0.f;
    };
};

class MetalFxResolveShader : public RHIShaderInfo
{
    REGISTGER_SHADER(MetalFxResolveShader, RHIShaderStage::Compute, "shaders/metalfx/metalfx_resolve.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(scalerOutput, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(sceneAccum, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outColor, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt output_resolution;
        Vector2UInt input_resolution;
        float handoff_weight = 0.f;
        float padding0 = 0.f;
        float padding1 = 0.f;
        float padding2 = 0.f;
    };
};

struct MetalFxDenoiser::Impl
{
    explicit Impl(RHIContext *in_rhi, const DenoiserDesc &in_desc)
        : rhi(in_rhi), desc(in_desc), timings(in_rhi, "MetalFxPerf", in_desc.max_frames_in_flight)
    {
    }

    RHIResourceRef<MetalImage> CreatePreparedTexture(PixelFormat format, MTLTextureUsage required_usage, bool writable,
                                                     Vector2UInt size, const char *name)
    {
        if (!HasOnlyKnownUsages(required_usage, name))
        {
            return nullptr;
        }

        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:GetMetalPixelFormat(format)
                                                               width:size.x()
                                                              height:size.y()
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = required_usage | MTLTextureUsageShaderRead;
        if (writable)
        {
            descriptor.usage |= MTLTextureUsageShaderWrite;
        }

        id<MTLTexture> texture = [context->GetDevice() newTextureWithDescriptor:descriptor];
        if (!texture)
        {
            Log(Error, "MetalFX: failed to allocate {}", name);
            return nullptr;
        }
        SetDebugInfo(texture, name);

        RHIImage::Attribute attribute{.format = format,
                                      .width = size.x(),
                                      .height = size.y(),
                                      .usages = writable ? RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV
                                                         : RHIImage::ImageUsage::Texture,
                                      .memory_properties = RHIMemoryProperty::DeviceLocal};
        return rhi->CreateResource<MetalImage>(attribute, texture, name);
    }

    void CreatePipelines()
    {
        prepare_ubo = rhi->CreateBuffer({.size = sizeof(MetalFxPrepareShader::UniformBufferData),
                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                         .mem_properties = RHIMemoryProperty::None,
                                         .is_dynamic = true},
                                        "MetalFxPrepareUBO");
        prepare_shader = rhi->CreateShader<MetalFxPrepareShader>();
        prepare_pipeline = rhi->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "MetalFxPreparePipeline");
        prepare_pipeline->SetShader<RHIShaderStage::Compute>(prepare_shader);
        prepare_pipeline->Compile();
        prepare_pipeline->GetShaderResource<MetalFxPrepareShader>()->ubo().BindResource(prepare_ubo);
        prepare_pass = rhi->CreateComputePass("MetalFxPreparePass", true);

        resolve_ubo = rhi->CreateBuffer({.size = sizeof(MetalFxResolveShader::UniformBufferData),
                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                         .mem_properties = RHIMemoryProperty::None,
                                         .is_dynamic = true},
                                        "MetalFxResolveUBO");
        resolve_shader = rhi->CreateShader<MetalFxResolveShader>();
        resolve_pipeline = rhi->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "MetalFxResolvePipeline");
        resolve_pipeline->SetShader<RHIShaderStage::Compute>(resolve_shader);
        resolve_pipeline->Compile();
        resolve_pipeline->GetShaderResource<MetalFxResolveShader>()->ubo().BindResource(resolve_ubo);
        resolve_pass = rhi->CreateComputePass("MetalFxResolvePass", true);

        timings.AddStage("prepare", prepare_pass);
        timings.AddStage("resolve", resolve_pass);
    }

#if SPARKLE_HAS_METALFX_DENOISED
    // the scaler and its textures for the descriptor's extents, bound into the pipelines. returns whether the device
    // serves the extents.
    API_AVAILABLE(macos(26.0), ios(26.0)) bool CreateScaler()
    {
        scaler = nil;

        if (desc.input_size.x() == 0 || desc.input_size.y() == 0 || desc.output_size.x() == 0 ||
            desc.output_size.y() == 0)
        {
            Log(Error, "MetalFX: input and output extents must be nonzero");
            return false;
        }

        auto device = context->GetDevice();
        const float min_scale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device];
        const float max_scale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device];
        const float scale_x = static_cast<float>(desc.output_size.x()) / static_cast<float>(desc.input_size.x());
        const float scale_y = static_cast<float>(desc.output_size.y()) / static_cast<float>(desc.input_size.y());
        if (scale_x < min_scale || scale_x > max_scale || scale_y < min_scale || scale_y > max_scale)
        {
            Log(Info, "MetalFX: requested input scales [{}, {}] are outside supported range [{}, {}]", scale_x, scale_y,
                min_scale, max_scale);
            return false;
        }

        MTLFXTemporalDenoisedScalerDescriptor *descriptor = [[MTLFXTemporalDenoisedScalerDescriptor alloc] init];
        descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.depthTextureFormat = MTLPixelFormatR32Float;
        descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
        descriptor.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.normalTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.roughnessTextureFormat = MTLPixelFormatR16Float;
        descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.inputWidth = desc.input_size.x();
        descriptor.inputHeight = desc.input_size.y();
        descriptor.outputWidth = desc.output_size.x();
        descriptor.outputHeight = desc.output_size.y();
        descriptor.autoExposureEnabled = NO;
        descriptor.requiresSynchronousInitialization = desc.synchronous_initialization;
        descriptor.reactiveMaskTextureEnabled = NO;
        descriptor.specularHitDistanceTextureEnabled = NO;
        descriptor.denoiseStrengthMaskTextureEnabled = NO;
        descriptor.transparencyOverlayTextureEnabled = NO;

        id<MTLFXTemporalDenoisedScaler> new_scaler = [descriptor newTemporalDenoisedScalerWithDevice:device];
        if (!new_scaler)
        {
            Log(Info, "MetalFX: failed to create temporal denoised scaler");
            return false;
        }

        color = CreatePreparedTexture(PixelFormat::RGBAFloat16, new_scaler.colorTextureUsage, true, desc.input_size,
                                      "MetalFxColor");
        depth = CreatePreparedTexture(PixelFormat::R32Float, new_scaler.depthTextureUsage, true, desc.input_size,
                                      "MetalFxDepth");
        motion = CreatePreparedTexture(PixelFormat::RGFloat16, new_scaler.motionTextureUsage, true, desc.input_size,
                                       "MetalFxMotion");
        diffuse_albedo = CreatePreparedTexture(PixelFormat::RGBAFloat16, new_scaler.diffuseAlbedoTextureUsage, true,
                                               desc.input_size, "MetalFxDiffuseAlbedo");
        specular_albedo = CreatePreparedTexture(PixelFormat::RGBAFloat16, new_scaler.specularAlbedoTextureUsage, true,
                                                desc.input_size, "MetalFxSpecularAlbedo");
        normal = CreatePreparedTexture(PixelFormat::RGBAFloat16, new_scaler.normalTextureUsage, true, desc.input_size,
                                       "MetalFxNormal");
        roughness = CreatePreparedTexture(PixelFormat::R16Float, new_scaler.roughnessTextureUsage, true,
                                          desc.input_size, "MetalFxRoughness");
        // writable: while the scaler output is displayed, the render graph declares the scaler's write as a
        // storage write
        output = CreatePreparedTexture(PixelFormat::RGBAFloat16, new_scaler.outputTextureUsage, true, desc.output_size,
                                       "MetalFxOutput");
        if (!color || !depth || !motion || !diffuse_albedo || !specular_albedo || !normal || !roughness || !output)
        {
            return false;
        }

        resolved_output = rhi->CreateImage(
            RHIImage::Attribute{
                .format = PixelFormat::RGBAFloat16,
                .width = desc.output_size.x(),
                .height = desc.output_size.y(),
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
                .mip_levels = 1,
                .msaa_samples = 1,
            },
            "MetalFxResolvedOutput");

        auto *prepare_resources = prepare_pipeline->GetShaderResource<MetalFxPrepareShader>();
        prepare_resources->outColor().BindResource(color->GetDefaultView(rhi));
        prepare_resources->outDepth().BindResource(depth->GetDefaultView(rhi));
        prepare_resources->outMotion().BindResource(motion->GetDefaultView(rhi));
        prepare_resources->outDiffuseAlbedo().BindResource(diffuse_albedo->GetDefaultView(rhi));
        prepare_resources->outSpecularAlbedo().BindResource(specular_albedo->GetDefaultView(rhi));
        prepare_resources->outNormal().BindResource(normal->GetDefaultView(rhi));
        prepare_resources->outRoughness().BindResource(roughness->GetDefaultView(rhi));

        auto *resolve_resources = resolve_pipeline->GetShaderResource<MetalFxResolveShader>();
        resolve_resources->scalerOutput().BindResource(output->GetDefaultView(rhi));
        resolve_resources->outColor().BindResource(resolved_output->GetDefaultView(rhi));

        scaler = new_scaler;
        reset_history = true;
        Log(Info, "MetalFX: temporal denoised scaler ready, input [{} x {}], output [{} x {}]", desc.input_size.x(),
            desc.input_size.y(), desc.output_size.x(), desc.output_size.y());
        return true;
    }
#endif

    [[nodiscard]] DenoiserHandoff GetHandoff() const
    {
        return DenoiserHandoff(frame.maximum_samples);
    }

    [[nodiscard]] bool ValidInputs(const RenderGraph &graph, const DenoiserInputs &inputs) const
    {
        const auto valid_input = [this, &graph](RGTexture texture, PixelFormat format, const char *name) {
            const auto size = graph.GetSize(texture);
            if (size.x() == desc.input_size.x() && size.y() == desc.input_size.y() &&
                (format == PixelFormat::Count || graph.GetFormat(texture) == format))
            {
                return true;
            }

            Log(Error, "MetalFX: {} does not match the denoiser input descriptor", name);
            return false;
        };
        return valid_input(inputs.accumulated_radiance, PixelFormat::Count, "accumulated radiance") &&
               valid_input(inputs.normal_view_depth, PixelFormat::RGBAFloat, "normal and depth") &&
               valid_input(inputs.albedo_object_id, PixelFormat::RGBAFloat, "albedo and object ID") &&
               valid_input(inputs.motion_hit_metallic, PixelFormat::RGBAFloat16, "motion and hit state") &&
               valid_input(inputs.specular_albedo_roughness, PixelFormat::RGBAFloat16, "specular albedo and roughness");
    }

    void BindInputs(const RGPassContext &pass_context, const DenoiserInputs &inputs)
    {
        const auto view = [this, &pass_context](RGTexture texture) {
            return pass_context.GetImage(texture)->GetDefaultView(rhi);
        };

        auto *resources = prepare_pipeline->GetShaderResource<MetalFxPrepareShader>();
        resources->sceneRadiance().BindResource(view(inputs.accumulated_radiance), true);
        resources->normalViewDepth().BindResource(view(inputs.normal_view_depth), true);
        resources->albedoObjectId().BindResource(view(inputs.albedo_object_id), true);
        resources->motionHitMetallic().BindResource(view(inputs.motion_hit_metallic), true);
        resources->specularAlbedoRoughness().BindResource(view(inputs.specular_albedo_roughness), true);
    }

    RHIContext *rhi;
    DenoiserDesc desc;
    DenoiserFrameData frame;
    float uploaded_exposure = -1.f;
    bool ready = false;
    bool reset_history = true;

    id scaler = nil;
    id<MTLTexture> exposure_texture = nil;

    RHIResourceRef<MetalImage> color;
    RHIResourceRef<MetalImage> depth;
    RHIResourceRef<MetalImage> motion;
    RHIResourceRef<MetalImage> diffuse_albedo;
    RHIResourceRef<MetalImage> specular_albedo;
    RHIResourceRef<MetalImage> normal;
    RHIResourceRef<MetalImage> roughness;
    RHIResourceRef<MetalImage> output;
    RHIResourceRef<RHIImage> resolved_output;

    RHIResourceRef<RHIBuffer> prepare_ubo;
    RHIResourceRef<RHIShader> prepare_shader;
    RHIResourceRef<RHIPipelineState> prepare_pipeline;
    RHIResourceRef<RHIComputePass> prepare_pass;

    RHIResourceRef<RHIBuffer> resolve_ubo;
    RHIResourceRef<RHIShader> resolve_shader;
    RHIResourceRef<RHIPipelineState> resolve_pipeline;
    RHIResourceRef<RHIComputePass> resolve_pass;

    PassTimingAggregator timings;
    bool display_scaler_output = false;
};

MetalFxDenoiser::MetalFxDenoiser(RHIContext *rhi, const DenoiserDesc &desc) : impl_(std::make_unique<Impl>(rhi, desc))
{
#if SPARKLE_HAS_METALFX_DENOISED
    if (@available(macOS 26.0, iOS 26.0, *))
    {
        auto device = context->GetDevice();
        if (![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device])
        {
            Log(Info, "MetalFX: temporal denoised scaling is unsupported by this device");
            return;
        }

        MTLTextureDescriptor *exposure_descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        exposure_descriptor.storageMode = MTLStorageModeShared;
        exposure_descriptor.usage = MTLTextureUsageShaderRead;
        impl_->exposure_texture = [device newTextureWithDescriptor:exposure_descriptor];
        if (!impl_->exposure_texture)
        {
            Log(Error, "MetalFX: failed to allocate exposure texture");
            return;
        }
        impl_->exposure_texture.label = @"MetalFxExposure";

        impl_->CreatePipelines();
        impl_->ready = impl_->CreateScaler();
    }
    else
    {
        Log(Info, "MetalFX: temporal denoised scaling requires macOS 26 or iOS 26");
    }
#else
    (void)desc;
#if defined(SPARKLE_DISABLE_METALFX)
    Log(Info, "MetalFX: temporal denoised scaling is unavailable on the iOS simulator");
#else
    Log(Info, "MetalFX: temporal denoised scaler header is unavailable in this SDK");
#endif
#endif
}

MetalFxDenoiser::~MetalFxDenoiser()
{
    if (impl_->ready)
    {
        impl_->timings.LogTimings("final");
    }
}

bool MetalFxDenoiser::IsReady() const
{
    return impl_->ready;
}

bool MetalFxDenoiser::NeedsInputs() const
{
    const DenoiserHandoff handoff = impl_->GetHandoff();
    return impl_->ready &&
           (!handoff.Applies() || static_cast<float>(impl_->frame.accumulated_samples) < handoff.GetEnd());
}

const char *MetalFxDenoiser::GetName() const
{
    return "MetalFX";
}

RHIResourceRef<RHIImage> MetalFxDenoiser::GetOutput() const
{
    if (impl_->display_scaler_output)
    {
        return impl_->output;
    }
    return impl_->resolved_output;
}

void MetalFxDenoiser::UpdateFrameData(const DenoiserFrameData &frame)
{
    impl_->frame = frame;
}

void MetalFxDenoiser::Resize(const Vector2UInt &input_size, const Vector2UInt &output_size)
{
    impl_->desc.input_size = input_size;
    impl_->desc.output_size = output_size;
#if SPARKLE_HAS_METALFX_DENOISED
    if (@available(macOS 26.0, iOS 26.0, *))
    {
        impl_->ready = impl_->CreateScaler();
    }
#endif
}

// the prepared textures and, while resolving, the scaler output stay private to the pass and keep their own
// transitions
RGTexture MetalFxDenoiser::AddTo(RenderGraph &graph, const DenoiserInputs &inputs)
{
    ASSERT(NeedsInputs());

    // the frame shows the accumulator, and the renderer selects another provider next frame
    if (!impl_->ValidInputs(graph, inputs))
    {
        impl_->ready = false;
        return inputs.accumulated_radiance;
    }

    // the displayed image is the scaler output until the handoff starts, then its resolve into the accumulator
    const float handoff_weight = impl_->GetHandoff().ComputeWeight(static_cast<float>(impl_->frame.accumulated_samples),
                                                                   impl_->frame.final_frame);
    impl_->display_scaler_output = handoff_weight <= 0.f;
    const auto output = impl_->display_scaler_output ? graph.Import("MetalFxOutput", impl_->output)
                                                     : graph.Import("MetalFxResolvedOutput", impl_->resolved_output);

    graph.AddExternalPass("MetalFx", [this, inputs, output, handoff_weight](RGBuilder &builder) {
        for (const auto input : {inputs.normal_view_depth, inputs.albedo_object_id, inputs.motion_hit_metallic,
                                 inputs.specular_albedo_roughness, inputs.accumulated_radiance})
        {
            builder.Sampled(input, RHIShaderStageMask::Compute);
        }
        builder.StorageWrite(output);
        return [this, inputs, handoff_weight](RGExternalContext &pass_context) {
            Encode(pass_context, inputs, handoff_weight);
        };
    });
    return output;
}

void MetalFxDenoiser::Encode(RGExternalContext &pass_context, const DenoiserInputs &inputs,
                             [[maybe_unused]] float handoff_weight)
{
    impl_->BindInputs(pass_context, inputs);

    const auto &size = impl_->desc.input_size;
    MetalFxPrepareShader::UniformBufferData ubo{
        .projection = impl_->frame.projection, .resolution = size, .far_depth = 1.f};
    impl_->prepare_ubo->Upload(impl_->rhi, &ubo);

    auto &command_context = static_cast<MetalCommandContext &>(pass_context.GetCommandContext());

    for (const auto &output : {impl_->color, impl_->depth, impl_->motion, impl_->diffuse_albedo, impl_->specular_albedo,
                               impl_->normal, impl_->roughness})
    {
        output->Transition(command_context, {.target_layout = RHIImageLayout::StorageWrite,
                                             .after_stage = RHIPipelineStage::Top,
                                             .before_stage = RHIPipelineStage::ComputeShader});
    }

    command_context.BeginComputePass(impl_->prepare_pass);
    command_context.DispatchCompute(impl_->prepare_pipeline, {size.x(), size.y(), 1u}, {16u, 16u, 1u});
    command_context.EndComputePass(impl_->prepare_pass);

    for (const auto &prepared : {impl_->color, impl_->depth, impl_->motion, impl_->diffuse_albedo,
                                 impl_->specular_albedo, impl_->normal, impl_->roughness})
    {
        prepared->Transition(command_context, {.target_layout = RHIImageLayout::Read,
                                               .after_stage = RHIPipelineStage::ComputeShader,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    }

#if SPARKLE_HAS_METALFX_DENOISED
    if (@available(macOS 26.0, iOS 26.0, *))
    {
        // a ready denoiser has a scaler
        id<MTLFXTemporalDenoisedScaler> scaler = impl_->scaler;
        ASSERT(scaler != nil);

        // CPU-writing a shared texture races frames in flight that still read it: only rewrite on change
        const float exposure_value = std::max(impl_->frame.exposure, 0.f);
        if (exposure_value != impl_->uploaded_exposure)
        {
            const Half exposure(exposure_value);
            [impl_->exposure_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                       mipmapLevel:0
                                         withBytes:&exposure
                                       bytesPerRow:sizeof(exposure)];
            impl_->uploaded_exposure = exposure_value;
        }

        scaler.colorTexture = impl_->color->GetResource();
        scaler.depthTexture = impl_->depth->GetResource();
        scaler.motionTexture = impl_->motion->GetResource();
        scaler.diffuseAlbedoTexture = impl_->diffuse_albedo->GetResource();
        scaler.specularAlbedoTexture = impl_->specular_albedo->GetResource();
        scaler.normalTexture = impl_->normal->GetResource();
        scaler.roughnessTexture = impl_->roughness->GetResource();
        scaler.outputTexture = impl_->output->GetResource();
        scaler.exposureTexture = impl_->exposure_texture;
        scaler.preExposure = 1.f;
        scaler.jitterOffsetX = 0.f;
        scaler.jitterOffsetY = 0.f;
        scaler.motionVectorScaleX = static_cast<float>(size.x());
        scaler.motionVectorScaleY = static_cast<float>(size.y());
        scaler.shouldResetHistory = impl_->reset_history || impl_->frame.reset_history;
        scaler.depthReversed = NO;
        scaler.worldToViewMatrix = ToSimdMatrix(impl_->frame.view);
        scaler.viewToClipMatrix = ToSimdMatrix(impl_->frame.projection);

        const bool run_resolve = !impl_->display_scaler_output;
        impl_->timings.Sample({true, run_resolve});

        command_context.AssertOutsidePass("MetalFX denoise");
        id<MTLCommandBuffer> command_buffer = command_context.GetCommandBuffer();
        [command_buffer pushDebugGroup:@"MetalFX temporal denoised scaler"];
        [scaler encodeToCommandBuffer:command_buffer];
        [command_buffer popDebugGroup];

        if (run_resolve)
        {
            impl_->output->Transition(command_context, {.target_layout = RHIImageLayout::Read,
                                                        .after_stage = RHIPipelineStage::ComputeShader,
                                                        .before_stage = RHIPipelineStage::ComputeShader});

            const auto &output_size = impl_->desc.output_size;
            MetalFxResolveShader::UniformBufferData resolve_ubo_data{
                .output_resolution = output_size, .input_resolution = size, .handoff_weight = handoff_weight};
            impl_->resolve_ubo->Upload(impl_->rhi, &resolve_ubo_data);

            auto *resolve_resources = impl_->resolve_pipeline->GetShaderResource<MetalFxResolveShader>();
            resolve_resources->sceneAccum().BindResource(
                pass_context.GetImage(inputs.accumulated_radiance)->GetDefaultView(impl_->rhi), true);

            command_context.BeginComputePass(impl_->resolve_pass);
            command_context.DispatchCompute(impl_->resolve_pipeline, {output_size.x(), output_size.y(), 1u},
                                            {16u, 16u, 1u});
            command_context.EndComputePass(impl_->resolve_pass);
        }
        impl_->reset_history = false;
    }
#endif
}

std::unique_ptr<Denoiser> CreateMetalFxDenoiser(RHIContext *rhi, const DenoiserDesc &desc)
{
    auto denoiser = std::make_unique<MetalFxDenoiser>(rhi, desc);
    if (!denoiser->IsReady())
    {
        return nullptr;
    }
    return denoiser;
}
} // namespace sparkle

#endif
