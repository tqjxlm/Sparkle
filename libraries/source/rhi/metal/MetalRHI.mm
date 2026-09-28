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

void MetalRHI::BeginCommandBuffer()
{
    context->BeginCommandBuffer();
}

RHICommandContext *MetalRHI::GetCommandContext()
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

RHIResourceRef<RHIShader> MetalRHI::CreateShader(const RHIShaderInfo *shader_info)
{
    return CreateResource<MetalShader>(shader_info);
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
