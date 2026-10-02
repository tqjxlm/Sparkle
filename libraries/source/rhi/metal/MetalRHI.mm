#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include "MetalBuffer.h"
#include "MetalComputePass.h"
#include "MetalContext.h"
#include "MetalImage.h"
#include "MetalNrdBackend.h"
#include "MetalPipelineState.h"
#include "MetalRayTracing.h"
#include "MetalResourceArray.h"
#include "MetalShader.h"
#include "MetalTimer.h"
#include "MetalUi.h"
#include "apple/AppleNativeView.h"
#include "core/Logger.h"

namespace sparkle
{
bool MetalRHI::InitRHI(NativeView *inWindow, std::string &error)
{
    @autoreleasepool
    {
        if (!RHIContext::InitRHI(inWindow, error))
        {
            return false;
        }

        auto *native_view = static_cast<AppleNativeView *>(inWindow);
        auto *metal_view = native_view->GetMetalView();

        int width = 0;
        int height = 0;
        native_view->GetFrameBufferSize(width, height);

        context = std::make_unique<MetalContext>(this, metal_view, IsHeadless(), static_cast<uint32_t>(width),
                                                 static_cast<uint32_t>(height));

        if (context->GetDevice() == nullptr)
        {
            error = "Unable to create a metal device. Please run with API validation in XCode to get more info.";
            return false;
        }

        initialization_success_ = true;

        return true;
    }
}

void MetalRHI::InitRenderResources()
{
    context->CreateBackBuffer();
}

RHIResourceRef<RHIImage> MetalRHI::GetBackBuffer() const
{
    return context->GetBackBufferColor();
}

void MetalRHI::CleanupInternal()
{
    initialization_success_ = false;
    context = nullptr;
}

void MetalRHI::WaitForDeviceIdle()
{
    context->WaitUntilDeviceIdle();
}

bool MetalRHI::SupportsHardwareRayTracing()
{
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    // root cause unknown: ASAN builds crash in acceleration structure code, so hardware ray tracing stays off
    Log(Warn, "Hardware ray tracing is incompatible with ASAN.");
    return false;
#endif
#endif
    return context->GetDevice().supportsRaytracing;
}

bool MetalRHI::SupportsPassTimestamps()
{
    return context->SupportsPassTimestamps();
}

// memoryless storage exists only on Apple family GPUs; the simulator creates depth textures only in private storage
bool MetalRHI::SupportsMemorylessImage(PixelFormat /*format*/, RHIImage::ImageUsage /*usages*/)
{
#if TARGET_OS_SIMULATOR
    return false;
#else
    return [context->GetDevice() supportsFamily:MTLGPUFamilyApple1];
#endif
}

// framebuffer fetch (programmable blending) exists only on Apple family GPUs; the simulator rejects reading a render
// target
bool MetalRHI::SupportsPixelLocalRead()
{
#if TARGET_OS_SIMULATOR
    return false;
#else
    return [context->GetDevice() supportsFamily:MTLGPUFamilyApple1];
#endif
}

// a pixel-local barrier records nothing
bool MetalRHI::KeepsMemorylessAcrossPixelLocalBarrier()
{
    return true;
}

// Metal feature set tables: the maximum implicit image block size per pixel when using multiple color render targets
std::optional<uint32_t> MetalRHI::GetTileBudget()
{
    id<MTLDevice> device = context->GetDevice();
    if ([device supportsFamily:MTLGPUFamilyApple7])
    {
        return 128;
    }
    if ([device supportsFamily:MTLGPUFamilyApple4])
    {
        return 64;
    }
    if ([device supportsFamily:MTLGPUFamilyApple2])
    {
        return 32;
    }
    return std::nullopt;
}

bool MetalRHI::SupportsSampledFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::ASTC4x4Srgb:
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC6x6Srgb:
    case PixelFormat::ASTC6x6Unorm:
        return [context->GetDevice() supportsFamily:MTLGPUFamilyApple2];
    case PixelFormat::ASTC4x4HDR:
        return [context->GetDevice() supportsFamily:MTLGPUFamilyApple6];
    case PixelFormat::BC7Srgb:
    case PixelFormat::BC7Unorm:
    case PixelFormat::BC6HUfloat:
#if FRAMEWORK_MACOS
        return context->GetDevice().supportsBCTextureCompression;
#else
        return false;
#endif
    default:
        return true;
    }
}

// Metal feature set tables: 32-bit float color formats and Depth32Float filter on Apple9 and later, and elsewhere only
// where the device reports supports32BitFloatFiltering; integer formats never filter. Depth24Unorm_Stencil8 exists only
// on some Mac GPUs and is treated as unfilterable.
bool MetalRHI::SupportsLinearFiltering(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::R32UInt:
    case PixelFormat::RGBAUInt32:
    case PixelFormat::D24S8:
        return false;
    case PixelFormat::R32Float:
    case PixelFormat::RGBAFloat:
    case PixelFormat::D32:
        return [context->GetDevice() supportsFamily:MTLGPUFamilyApple9] ||
               context->GetDevice().supports32BitFloatFiltering;
    default:
        return SupportsSampledFormat(format);
    }
}

bool MetalRHI::BeginFrameInternal()
{
    context->BeginFrame();
    return true;
}

void MetalRHI::EndFrameInternal()
{
    if (GetConfig().measure_gpu_time)
    {
        auto frame_index = GetFrameIndex();
        auto *command_context = context->GetCommandContext();
        ASSERT_F(command_context, "the frame ends outside its command buffer");
        [command_context->GetCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer> command_buffer) {
          frame_stats_[frame_index].elapsed_time_ms = (command_buffer.GPUEndTime - command_buffer.GPUStartTime) * 1e3f;
        }];
    }

    context->EndFrame();
}

void MetalRHI::SubmitCommandBuffer()
{
    context->SubmitCommandBuffer();
}

RHICommandContext &MetalRHI::BeginCommandBufferInternal()
{
    context->BeginCommandBuffer();
    return *context->GetCommandContext();
}

RHICommandContext *MetalRHI::GetCommandContextInternal()
{
    return context->GetCommandContext();
}

bool MetalRHI::RecreateSurface()
{
    UnImplemented();
    return true;
}

void MetalRHI::RecreateSwapChain()
{
    UnImplemented();
}

RHIResourceRef<RHIShader> MetalRHI::CreateShader(const RHIShaderInfo *shader_info, std::string variant)
{
    return CreateResource<MetalShader>(shader_info, std::move(variant));
}

RHIResourceRef<RHIPipelineState> MetalRHI::CreatePipelineState(RHIPipelineState::PipelineType type,
                                                               const std::string &name)
{
    switch (type)
    {
    case RHIPipelineState::PipelineType::Graphics:
        return CreateResource<MetalGraphicsPipeline>(type, name);
    case RHIPipelineState::PipelineType::Compute:
        return CreateResource<MetalComputePipeline>(type, name);
    }
}

RHIResourceRef<RHIBuffer> MetalRHI::CreateBuffer(const RHIBuffer::Attribute &attribute, const std::string &name)
{
    return CreateResource<MetalBuffer>(attribute, name);
}

RHIResourceRef<RHIImage> MetalRHI::CreateImage(const RHIImage::Attribute &attributes, const std::string &name)
{
    return CreateResource<MetalImage>(attributes, name);
}

RHIResourceRef<RHIImageView> MetalRHI::CreateImageView(RHIImage *image, const RHIImageView::Attribute &attribute)
{
    return CreateResource<MetalImageView>(attribute, image);
}

RHIResourceRef<RHIBLAS> MetalRHI::CreateBLAS(const TransformMatrix &transform,
                                             const RHIResourceRef<RHIBuffer> &vertex_buffer,
                                             const RHIResourceRef<RHIBuffer> &index_buffer, uint32_t num_primitive,
                                             uint32_t num_vertex, const std::string &name)
{
    return CreateResource<MetalBLAS>(transform, vertex_buffer, index_buffer, num_primitive, num_vertex, name);
}

RHIResourceRef<RHITLAS> MetalRHI::CreateTLAS(const std::string &name)
{
    return CreateResource<MetalTLAS>(name);
}

RHIResourceRef<RHISampler> MetalRHI::CreateSampler(RHISampler::SamplerAttribute attribute, const std::string &name)
{
    return CreateResource<MetalSampler>(attribute, name);
}

std::unique_ptr<RHINrdBackend> MetalRHI::CreateNrdBackend()
{
    return std::make_unique<MetalNrdBackend>(context->GetDevice());
}

RHIResourceRef<RHIUiHandler> MetalRHI::CreateUiHandler()
{
    return CreateResource<MetalUiHandler>();
}

void MetalRHI::CaptureNextFrames(int count)
{
    context->CaptureNextFrames(count);
}

RHIResourceRef<RHIResourceArray> MetalRHI::CreateResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                               unsigned capacity, const std::string &name)
{
    return CreateResource<MetalResourceArray>(type, capacity, name);
}

RHIResourceRef<RHITimer> MetalRHI::CreateTimer(const std::string &name)
{
    return CreateResource<MetalTimer>(name);
}

RHIResourceRef<RHIComputePass> MetalRHI::CreateComputePass(const std::string &name, bool need_timestamp)
{
    return CreateResource<MetalComputePass>(this, need_timestamp, name);
}
} // namespace sparkle

#endif
