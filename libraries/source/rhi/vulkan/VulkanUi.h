#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "rhi/RHIUiHandler.h"

#include <optional>

namespace sparkle
{
class VulkanUiHandler : public RHIUiHandler
{
public:
    explicit VulkanUiHandler();

    ~VulkanUiHandler() override;

    void BeginFrame(const RHIRenderingInfo &info) override;

    void Render(RHICommandContext &command_context) override;

private:
    void CreateDescriptorPool();

    // makes ImGui's main pipeline the one for `signature`
    void CompilePipeline(const RHIAttachmentSignature &signature);

    VkDescriptorPool descriptor_pool_ = nullptr;

    std::optional<RHIAttachmentSignature> pipeline_signature_;
};
} // namespace sparkle

#endif
