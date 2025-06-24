#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include "MetalBuffer.h"
#include "MetalComputePass.h"
#include "MetalContext.h"
#include "MetalImage.h"
#include "MetalPipelineState.h"
#include "MetalRayTracing.h"
#include "MetalRenderPass.h"
#include "MetalRenderTarget.h"
#include "MetalResourceArray.h"
#include "MetalShader.h"
#include "MetalTimer.h"
#include "MetalUi.h"
#include "apple/AppleNativeView.h"
#include "core/Logger.h"

namespace sparkle
{
static MTLPrimitiveType GetMetalPrimitiveType(RHIPipelineState::PolygonMode mode)
{
    switch (mode)
    {
    case RHIPipelineState::PolygonMode::Fill:
        return MTLPrimitiveTypeTriangle;
    case RHIPipelineState::PolygonMode::Line:
        return MTLPrimitiveTypeLine;
    case RHIPipelineState::PolygonMode::Point:
        return MTLPrimitiveTypePoint;
    default:
        UnImplemented(mode);
    }
}

bool MetalRHI::InitRHI(NativeView *inWindow, std::string &error)
{
    @autoreleasepool
    {
        if (!RHIContext::InitRHI(inWindow, error))
        {
            return false;
        }

        auto *metal_view = static_cast<AppleNativeView *>(inWindow)->GetMetalView();
        context = std::make_unique<MetalContext>(this, metal_view);

        if (context->GetDevice() == nullptr)
        {
            error = "Unable to create a metal device. Please run with API validation in XCode to get more info.";
            return false;
        }

        initialization_success_ = true;

        return true;
    }
}

static auto CreateBackBufferDepth(CGSize extent)
{
    RHIImage::Attribute attribute;
    attribute.width = extent.width;
    attribute.height = extent.height;
    attribute.mip_levels = 1;
    attribute.msaa_samples = 1;
    attribute.format = PixelFormat::D32;
    attribute.usages = RHIImage::ImageUsage::DepthStencilAttachment | RHIImage::ImageUsage::TransientAttachment;
    attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                         .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                         .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                         .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};
    attribute.memory_properties = RHIMemoryProperty::DeviceLocal;

    return context->GetRHI()->CreateResource<MetalImage>(attribute, "BackBufferDepth");
}

void MetalRHI::InitRenderResources()
{
    context->CreateBackBuffer();

    back_buffer_rt_ =
        CreateBackBufferRenderTarget({}, CreateBackBufferDepth(context->GetView().drawableSize), "BackBufferRT");
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
    // TODO(tqjxlm): we do not know why ASAN causes memory issues for acceleration structures. disable for now.
    Log(Warn, "Hardware ray tracing is incompatible with ASAN.");
    return false;
#endif
#endif
    return context->GetDevice().supportsRaytracing;
}

void MetalRHI::BeginFrameInternal()
{
    context->BeginFrame();
}

void MetalRHI::EndFrameInternal()
{
    if (GetConfig().measure_gpu_time)
    {
        auto frame_index = GetFrameIndex();
        [context->GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer> command_buffer) {
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

bool MetalRHI::RecreateSurface()
{
    UnImplemented();
    return true;
}

void MetalRHI::RecreateSwapChain()
{
    UnImplemented();
}

void MetalRHI::NextSubpass()
{
    UnImplemented();
}

void MetalRHI::DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args)
{
    auto *pso = RHICast<MetalGraphicsPipeline>(pipeline_state);
    auto *pass = RHICast<MetalRenderPass>(current_render_pass_);
    auto encoder = pass->GetRenderEncoder();

    auto index_buffer = RHICast<MetalBuffer>(pso->GetIndexBuffer())->GetResource();

    pso->Bind(encoder);

    [encoder drawIndexedPrimitives:GetMetalPrimitiveType(pipeline_state->GetRasterizationState().polygon_mode)
                        indexCount:draw_args.index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:index_buffer
                 indexBufferOffset:draw_args.first_index
                     instanceCount:draw_args.instance_count
                        baseVertex:draw_args.first_vertex
                      baseInstance:draw_args.first_instance];
}

void MetalRHI::DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                               Vector3UInt thread_per_group)
{
    auto *pass = RHICast<MetalComputePass>(current_compute_pass_);
    ASSERT(pass);

    auto *pso = RHICast<MetalComputePipeline>(pipeline);

    auto encoder = pass->GetEncoder();

    pso->Bind(encoder);

    MTLSize grid_size = MTLSizeMake(total_threads.x(), total_threads.y(), total_threads.z());
    MTLSize threadgroup_size = MTLSizeMake(thread_per_group.x(), thread_per_group.y(), thread_per_group.z());

    [encoder dispatchThreads:grid_size threadsPerThreadgroup:threadgroup_size];
}

RHIResourceRef<RHIRenderTarget> MetalRHI::CreateBackBufferRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                                       const RHIResourceRef<RHIImage> &depth_image,
                                                                       const std::string &name)
{
    return CreateResource<MetalRenderTarget>(attribute, depth_image, name);
}

RHIResourceRef<RHIRenderTarget> MetalRHI::CreateRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                             const RHIRenderTarget::ColorImageArray &color_images,
                                                             const RHIResourceRef<RHIImage> &depth_image,
                                                             const std::string &name)
{
    return CreateResource<MetalRenderTarget>(attribute, color_images, depth_image, name);
}

RHIResourceRef<RHIRenderPass> MetalRHI::CreateRenderPass(const RHIRenderPass::Attribute &attribute,
                                                         const RHIResourceRef<RHIRenderTarget> &rt,
                                                         const std::string &name)
{
    return CreateResource<MetalRenderPass>(attribute, rt, name);
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

void MetalRHI::BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass)
{
    RHICast<MetalRenderPass>(pass)->Begin();
}

void MetalRHI::EndRenderPassInternal()
{
    RHICast<MetalRenderPass>(current_render_pass_)->End();
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

void MetalRHI::BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass)
{
    RHICast<MetalComputePass>(pass)->Begin();
}

void MetalRHI::EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass)
{
    RHICast<MetalComputePass>(pass)->End();
}
} // namespace sparkle

#endif
