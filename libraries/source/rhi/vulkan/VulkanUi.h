#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class VulkanUiHandler : public RHIUiHandler
{
public:
    explicit VulkanUiHandler();

    ~VulkanUiHandler() override;

    void BeginFrame() override;

    void Render() override;

    void Init() override;

private:
    void CreateDescriptorPool();

    VkDescriptorPool descriptor_pool_ = nullptr;

    bool initialized_ = false;
};
} // namespace sparkle

#endif
