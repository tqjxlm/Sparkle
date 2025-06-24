#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHI.h"

namespace sparkle
{
class MetalRHI final : public RHIContext
{
public:
    using RHIContext::RHIContext;

    ~MetalRHI() override = default;

    bool InitRHI(NativeView *inWindow, std::string &error) override;
    void InitRenderResources() override;
    void WaitForDeviceIdle() override;
    void CaptureNextFrames(int count) override;

    bool SupportsHardwareRayTracing() override;

    void BeginCommandBuffer() override;
    void SubmitCommandBuffer() override;

    bool RecreateSurface() override;
    void RecreateSwapChain() override;
    void NextSubpass() override;

    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args) override;
    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group) override;

    RHIResourceRef<RHIRenderTarget> CreateBackBufferRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                                 const RHIResourceRef<RHIImage> &depth_image,
                                                                 const std::string &name) override;

    RHIResourceRef<RHIRenderTarget> CreateRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                       const RHIRenderTarget::ColorImageArray &color_images,
                                                       const RHIResourceRef<RHIImage> &depth_image,
                                                       const std::string &name) override;

    RHIResourceRef<RHIRenderPass> CreateRenderPass(const RHIRenderPass::Attribute &attribute,
                                                   const RHIResourceRef<RHIRenderTarget> &rt,
                                                   const std::string &name) override;

    RHIResourceRef<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineType type,
                                                         const std::string &name) override;

    RHIResourceRef<RHIBuffer> CreateBuffer(const RHIBuffer::Attribute &attribute, const std::string &name) override;
    RHIResourceRef<RHIImage> CreateImage(const RHIImage::Attribute &attributes, const std::string &name) override;
    RHIResourceRef<RHIImageView> CreateImageView(RHIImage *image, const RHIImageView::Attribute &attribute) override;

    RHIResourceRef<RHIBLAS> CreateBLAS(const TransformMatrix &transform, const RHIResourceRef<RHIBuffer> &vertex_buffer,
                                       const RHIResourceRef<RHIBuffer> &index_buffer, uint32_t num_primitive,
                                       uint32_t num_vertex, const std::string &name) override;
    RHIResourceRef<RHITLAS> CreateTLAS(const std::string &name) override;
    RHIResourceRef<RHIUiHandler> CreateUiHandler() override;

    RHIResourceRef<RHISampler> CreateSampler(RHISampler::SamplerAttribute attribute, const std::string &name) override;

    RHIResourceRef<RHIResourceArray> CreateResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                         unsigned capacity, const std::string &name) override;

    RHIResourceRef<RHITimer> CreateTimer(const std::string &name) override;

    RHIResourceRef<RHIComputePass> CreateComputePass(const std::string &name, bool need_timestamp) override;

protected:
    void BeginFrameInternal() override;
    void EndFrameInternal() override;

    void BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass) override;
    void EndRenderPassInternal() override;

    void BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;
    void EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;

    void CleanupInternal() override;

    RHIResourceRef<RHIShader> CreateShader(const RHIShaderInfo *shader_info) override;
};
} // namespace sparkle
#endif
