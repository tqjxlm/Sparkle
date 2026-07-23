#if FRAMEWORK_APPLE

#include "MetalFxDenoiser.h"

#include "MetalContext.h"
#include "MetalImage.h"
#include "MetalRHIInternal.h"
#include "core/Logger.h"
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

RHISampler::SamplerAttribute GetPreparedSampler()
{
    return {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
            .filtering_method_min = RHISampler::FilteringMethod::Nearest,
            .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};
}

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
    USE_SHADER_RESOURCE(noisyRadianceHitDistance, RHIShaderResourceReflection::ResourceType::Texture2D)
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

struct MetalFxDenoiser::Impl
{
    explicit Impl(RHIContext *in_rhi, const RHIDenoiserDesc &in_desc) : rhi(in_rhi), desc(in_desc)
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
                                      .sampler = GetPreparedSampler(),
                                      .width = size.x(),
                                      .height = size.y(),
                                      .usages = writable ? RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV
                                                         : RHIImage::ImageUsage::Texture,
                                      .memory_properties = RHIMemoryProperty::DeviceLocal};
        return rhi->CreateResource<MetalImage>(attribute, texture, name);
    }

    void CreatePreparePipeline()
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

        auto *resources = prepare_pipeline->GetShaderResource<MetalFxPrepareShader>();
        resources->ubo().BindResource(prepare_ubo);
        resources->outColor().BindResource(color->GetDefaultView(rhi));
        resources->outDepth().BindResource(depth->GetDefaultView(rhi));
        resources->outMotion().BindResource(motion->GetDefaultView(rhi));
        resources->outDiffuseAlbedo().BindResource(diffuse_albedo->GetDefaultView(rhi));
        resources->outSpecularAlbedo().BindResource(specular_albedo->GetDefaultView(rhi));
        resources->outNormal().BindResource(normal->GetDefaultView(rhi));
        resources->outRoughness().BindResource(roughness->GetDefaultView(rhi));

        prepare_pass = rhi->CreateComputePass("MetalFxPreparePass", true);
    }

    bool BindInputs(const RHIDenoiserInputs &inputs)
    {
        if (!inputs.noisy_radiance_hit_distance || !inputs.normal_view_depth || !inputs.albedo_object_id ||
            !inputs.motion_hit_metallic || !inputs.specular_albedo_roughness)
        {
            Log(Error, "MetalFX: missing required path-tracing input");
            return false;
        }

        const auto valid_input = [this](const RHIImage *image, PixelFormat format, const char *name) {
            const auto attributes = image->GetAttributes();
            if (attributes.width == desc.input_size.x() && attributes.height == desc.input_size.y() &&
                attributes.format == format)
            {
                return true;
            }

            Log(Error, "MetalFX: {} does not match the denoiser input descriptor", name);
            return false;
        };
        if (!valid_input(inputs.noisy_radiance_hit_distance, desc.radiance_format, "noisy radiance") ||
            !valid_input(inputs.normal_view_depth, PixelFormat::RGBAFloat, "normal and depth") ||
            !valid_input(inputs.albedo_object_id, PixelFormat::RGBAFloat, "albedo and object ID") ||
            !valid_input(inputs.motion_hit_metallic, PixelFormat::RGBAFloat16, "motion and hit state") ||
            !valid_input(inputs.specular_albedo_roughness, PixelFormat::RGBAFloat16, "specular albedo and roughness"))
        {
            return false;
        }

        auto *resources = prepare_pipeline->GetShaderResource<MetalFxPrepareShader>();
        resources->noisyRadianceHitDistance().BindResource(inputs.noisy_radiance_hit_distance->GetDefaultView(rhi),
                                                           true);
        resources->normalViewDepth().BindResource(inputs.normal_view_depth->GetDefaultView(rhi), true);
        resources->albedoObjectId().BindResource(inputs.albedo_object_id->GetDefaultView(rhi), true);
        resources->motionHitMetallic().BindResource(inputs.motion_hit_metallic->GetDefaultView(rhi), true);
        resources->specularAlbedoRoughness().BindResource(inputs.specular_albedo_roughness->GetDefaultView(rhi), true);
        return true;
    }

    RHIContext *rhi;
    RHIDenoiserDesc desc;
    RHIDenoiserFrameData frame;
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

    RHIResourceRef<RHIBuffer> prepare_ubo;
    RHIResourceRef<RHIShader> prepare_shader;
    RHIResourceRef<RHIPipelineState> prepare_pipeline;
    RHIResourceRef<RHIComputePass> prepare_pass;
};

MetalFxDenoiser::MetalFxDenoiser(RHIContext *rhi, const RHIDenoiserDesc &desc)
    : impl_(std::make_unique<Impl>(rhi, desc))
{
#if SPARKLE_HAS_METALFX_DENOISED
    if (@available(macOS 26.0, iOS 18.0, *))
    {
        if (desc.input_size.x() == 0 || desc.input_size.y() == 0 || desc.output_size.x() == 0 ||
            desc.output_size.y() == 0)
        {
            Log(Error, "MetalFX: input and output extents must be nonzero");
            return;
        }

        auto device = context->GetDevice();
        if (![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device])
        {
            Log(Info, "MetalFX: temporal denoised scaling is unsupported by this device");
            return;
        }

        const float min_scale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device];
        const float max_scale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device];
        const float scale_x = static_cast<float>(desc.input_size.x()) / static_cast<float>(desc.output_size.x());
        const float scale_y = static_cast<float>(desc.input_size.y()) / static_cast<float>(desc.output_size.y());
        if (scale_x < min_scale || scale_x > max_scale || scale_y < min_scale || scale_y > max_scale)
        {
            Log(Info, "MetalFX: requested input scales [{}, {}] are outside supported range [{}, {}]", scale_x, scale_y,
                min_scale, max_scale);
            return;
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

        id<MTLFXTemporalDenoisedScaler> scaler = [descriptor newTemporalDenoisedScalerWithDevice:device];
        if (!scaler)
        {
            Log(Info, "MetalFX: failed to create temporal denoised scaler");
            return;
        }
        impl_->scaler = scaler;

        impl_->color = impl_->CreatePreparedTexture(PixelFormat::RGBAFloat16, scaler.colorTextureUsage, true,
                                                    desc.input_size, "MetalFxColor");
        impl_->depth = impl_->CreatePreparedTexture(PixelFormat::R32Float, scaler.depthTextureUsage, true,
                                                    desc.input_size, "MetalFxDepth");
        impl_->motion = impl_->CreatePreparedTexture(PixelFormat::RGFloat16, scaler.motionTextureUsage, true,
                                                     desc.input_size, "MetalFxMotion");
        impl_->diffuse_albedo = impl_->CreatePreparedTexture(PixelFormat::RGBAFloat16, scaler.diffuseAlbedoTextureUsage,
                                                             true, desc.input_size, "MetalFxDiffuseAlbedo");
        impl_->specular_albedo =
            impl_->CreatePreparedTexture(PixelFormat::RGBAFloat16, scaler.specularAlbedoTextureUsage, true,
                                         desc.input_size, "MetalFxSpecularAlbedo");
        impl_->normal = impl_->CreatePreparedTexture(PixelFormat::RGBAFloat16, scaler.normalTextureUsage, true,
                                                     desc.input_size, "MetalFxNormal");
        impl_->roughness = impl_->CreatePreparedTexture(PixelFormat::R16Float, scaler.roughnessTextureUsage, true,
                                                        desc.input_size, "MetalFxRoughness");
        impl_->output = impl_->CreatePreparedTexture(PixelFormat::RGBAFloat16, scaler.outputTextureUsage, false,
                                                     desc.output_size, "MetalFxOutput");

        if (!impl_->color || !impl_->depth || !impl_->motion || !impl_->diffuse_albedo || !impl_->specular_albedo ||
            !impl_->normal || !impl_->roughness || !impl_->output)
        {
            impl_->scaler = nil;
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
            impl_->scaler = nil;
            return;
        }
        impl_->exposure_texture.label = @"MetalFxExposure";

        impl_->CreatePreparePipeline();
        impl_->ready = true;
        Log(Info, "MetalFX: temporal denoised scaler ready, input [{} x {}], output [{} x {}]", desc.input_size.x(),
            desc.input_size.y(), desc.output_size.x(), desc.output_size.y());
    }
    else
    {
        Log(Info, "MetalFX: temporal denoised scaling requires macOS 26 or iOS 18");
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

MetalFxDenoiser::~MetalFxDenoiser() = default;

bool MetalFxDenoiser::IsReady() const
{
    return impl_->ready;
}

bool MetalFxDenoiser::NeedsInputs() const
{
    return impl_->ready && !impl_->frame.final_frame;
}

const char *MetalFxDenoiser::GetName() const
{
    return "MetalFX";
}

RHIResourceRef<RHIImage> MetalFxDenoiser::GetOutput() const
{
    return impl_->output;
}

void MetalFxDenoiser::UpdateFrameData(const RHIDenoiserFrameData &frame)
{
    impl_->frame = frame;
}

bool MetalFxDenoiser::Encode(const RHIDenoiserInputs &inputs)
{
    if (!NeedsInputs() || !impl_->BindInputs(inputs))
    {
        return false;
    }

    const auto &size = impl_->desc.input_size;
    MetalFxPrepareShader::UniformBufferData ubo{
        .projection = impl_->frame.projection, .resolution = size, .far_depth = 1.f};
    impl_->prepare_ubo->Upload(impl_->rhi, &ubo);

    for (RHIImage *input : {inputs.noisy_radiance_hit_distance, inputs.normal_view_depth, inputs.albedo_object_id,
                            inputs.motion_hit_metallic, inputs.specular_albedo_roughness})
    {
        input->Transition({.target_layout = RHIImageLayout::Read,
                           .after_stage = RHIPipelineStage::ComputeShader,
                           .before_stage = RHIPipelineStage::ComputeShader});
    }
    for (const auto &output : {impl_->color, impl_->depth, impl_->motion, impl_->diffuse_albedo, impl_->specular_albedo,
                               impl_->normal, impl_->roughness})
    {
        output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                            .after_stage = RHIPipelineStage::Top,
                            .before_stage = RHIPipelineStage::ComputeShader});
    }

    impl_->rhi->BeginComputePass(impl_->prepare_pass);
    impl_->rhi->DispatchCompute(impl_->prepare_pipeline, {size.x(), size.y(), 1u}, {16u, 16u, 1u});
    impl_->rhi->EndComputePass(impl_->prepare_pass);

    for (const auto &prepared : {impl_->color, impl_->depth, impl_->motion, impl_->diffuse_albedo,
                                 impl_->specular_albedo, impl_->normal, impl_->roughness})
    {
        prepared->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::ComputeShader,
                              .before_stage = RHIPipelineStage::ComputeShader});
    }

#if SPARKLE_HAS_METALFX_DENOISED
    if (@available(macOS 26.0, iOS 18.0, *))
    {
        id<MTLFXTemporalDenoisedScaler> scaler = impl_->scaler;
        if (!scaler)
        {
            return false;
        }

        const Half exposure(std::max(impl_->frame.exposure, 0.f));
        [impl_->exposure_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                   mipmapLevel:0
                                     withBytes:&exposure
                                   bytesPerRow:sizeof(exposure)];

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
        scaler.jitterOffsetX = impl_->frame.jitter.x();
        scaler.jitterOffsetY = impl_->frame.jitter.y();
        scaler.motionVectorScaleX = static_cast<float>(size.x());
        scaler.motionVectorScaleY = static_cast<float>(size.y());
        scaler.shouldResetHistory = impl_->reset_history || impl_->frame.reset_history;
        scaler.depthReversed = NO;
        scaler.worldToViewMatrix = ToSimdMatrix(impl_->frame.view);
        scaler.viewToClipMatrix = ToSimdMatrix(impl_->frame.projection);

        id<MTLCommandBuffer> command_buffer = context->GetCurrentCommandBuffer();
        [command_buffer pushDebugGroup:@"MetalFX temporal denoised scaler"];
        [scaler encodeToCommandBuffer:command_buffer];
        [command_buffer popDebugGroup];

        impl_->output->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::PixelShader});
        impl_->reset_history = false;
        return true;
    }
#endif
    return false;
}
} // namespace sparkle

#endif
