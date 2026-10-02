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
    [[nodiscard]] RHIResourceRef<RHIImage> GetBackBuffer() const override;
    void WaitForDeviceIdle() override;
    void CaptureNextFrames(int count) override;

    bool SupportsHardwareRayTracing() override;

    bool SupportsPassTimestamps() override;

    bool SupportsMemorylessImage(PixelFormat format, RHIImage::ImageUsage usages) override;

    bool SupportsPixelLocalRead() override;

    bool KeepsMemorylessAcrossPixelLocalBarrier() override;

    std::optional<uint32_t> GetTileBudget() override;

    bool SupportsSampledFormat(PixelFormat format) override;

    bool SupportsLinearFiltering(PixelFormat format) override;

    bool HasPhysicalGpu() override
    {
        return true;
    }

    void SubmitCommandBuffer() override;

    bool RecreateSurface() override;
    void RecreateSwapChain() override;
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

    std::unique_ptr<RHINrdBackend> CreateNrdBackend() override;

protected:
    [[nodiscard]] bool BeginFrameInternal() override;
    void EndFrameInternal() override;

    void CleanupInternal() override;

    RHICommandContext *GetCommandContextInternal() override;

    RHICommandContext &BeginCommandBufferInternal() override;

    RHIResourceRef<RHIShader> CreateShader(const RHIShaderInfo *shader_info, std::string variant) override;
};
} // namespace sparkle
#endif
