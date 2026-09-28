#pragma once

#if ENABLE_VULKAN

#include "rhi/RHI.h"

#ifndef VULKAN_USE_VOLK
#define VULKAN_USE_VOLK 0
#endif

#if VULKAN_USE_VOLK
#define VK_NO_PROTOTYPES
#include <volk.h>
#else
#include <vulkan/vulkan.h>

namespace sparkle
{
// USAGE: when a function is not provided by vulkan SDK statically, declare it here and load it in VulkanFunctionLoader

// NOTICE: it relies on the fact that names from local namespace will override the global ones.
// any usage outside of "sparkle" namepsace will result in linking errors.

// NOLINTBEGIN
inline PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHR;
inline PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
inline PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
inline PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
inline PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR;
inline PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
inline PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT;
// NOLINTEND
}; // namespace sparkle
#endif

namespace sparkle
{
class DrawableNode;

class VulkanRHI final : public RHIContext
{
public:
    using RHIContext::RHIContext;
    ~VulkanRHI() override = default;
    void CreateBackBuffer();

    [[nodiscard]] RHIResourceRef<RHIImage> GetBackBuffer() const override;

    bool InitRHI(NativeView *inWindow, std::string &error) override;
    void InitRenderResources() override;
    void DestroySurface() override;
    bool RecreateSurface() override;
    void RecreateSwapChain() override;
    void ReleaseRenderResources() override;

    bool SupportsHardwareRayTracing() override;

    bool SupportsPassTimestamps() override;

    bool HasPhysicalGpu() override;

    bool SupportsSampledFormat(PixelFormat format) override;

    bool SupportsLinearFiltering(PixelFormat format) override;

    [[nodiscard]] std::optional<unsigned> GetValidationErrorCount() const override;

    [[nodiscard]] bool IsSyncValidationActive() const override;

    [[nodiscard]] uint32_t GetMinBufferOffsetAlignment() const override;

    void SubmitCommandBuffer() override;

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

    RHIResourceRef<RHIResourceArray> CreateResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                         unsigned int capacity, const std::string &name) override;

    RHIResourceRef<RHITimer> CreateTimer(const std::string &name) override;

    RHIResourceRef<RHIComputePass> CreateComputePass(const std::string &name, bool need_timestamp) override;

    std::unique_ptr<RHINrdBackend> CreateNrdBackend() override;

    void WaitForDeviceIdle() override;

protected:
    [[nodiscard]] bool BeginFrameInternal() override;
    void EndFrameInternal() override;

    void CleanupInternal() override;

    RHICommandContext *GetCommandContextInternal() override;

    RHICommandContext &BeginCommandBufferInternal() override;

    RHIResourceRef<RHISampler> CreateSampler(RHISampler::SamplerAttribute attribute, const std::string &name) override;

    RHIResourceRef<RHIShader> CreateShader(const RHIShaderInfo *shader_info) override;

private:
    std::vector<RHIResourceRef<RHITimer>> frame_timers_;
    // a windowed back buffer is the swap chain's
    RHIResourceRef<RHIImage> headless_back_buffer_;
};
} // namespace sparkle

#endif
