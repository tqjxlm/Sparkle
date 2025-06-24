#if ENABLE_VULKAN

#include "VulkanUi.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanRenderPass.h"
#include "VulkanSwapChain.h"

#include <imgui_impl_vulkan.h>

namespace sparkle
{
VulkanUiHandler::VulkanUiHandler() : RHIUiHandler("VulkanUiHandler")
{
#if defined(VK_NO_PROTOTYPES)
    auto func_loader = [](const char *func_name, void * /*handler*/) {
        PFN_vkVoidFunction instance_addr = vkGetInstanceProcAddr(context->GetInstance(), func_name);
        if (instance_addr)
        {
            return instance_addr;
        }
        PFN_vkVoidFunction device_addr = vkGetDeviceProcAddr(context->GetDevice(), func_name);
        return device_addr;
    };
    ImGui_ImplVulkan_LoadFunctions(0, func_loader, this);
#endif

    CreateDescriptorPool();

    is_valid_ = true;
}

void VulkanUiHandler::Init()
{
    if (initialized_)
    {
        ImGui_ImplVulkan_Shutdown();
    }

    QueueFamilyIndices const indices = FindQueueFamilies(context->GetPhysicalDevice(), context->GetSurface());

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = context->GetInstance();
    init_info.PhysicalDevice = context->GetPhysicalDevice();
    init_info.Device = context->GetDevice();
    init_info.QueueFamily = indices.graphicsFamily;
    init_info.Queue = context->GetGraphicsQueue();
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = descriptor_pool_;
    init_info.RenderPass = RHICast<VulkanRenderPass>(render_pass_)->GetRenderPass();
    init_info.Subpass = 0;
    init_info.MinImageCount = 2;
    init_info.ImageCount = context->GetRHI()->GetMaxFramesInFlight();
    init_info.MSAASamples = GetVkMsaaSampleBit(context->GetRHI()->GetConfig().msaa_samples);
    init_info.Allocator = VK_NULL_HANDLE;
    init_info.CheckVkResultFn = CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    // manually touch resources
    ImGui_ImplVulkan_CreateFontsTexture();

    initialized_ = true;
}

void VulkanUiHandler::BeginFrame()
{
    ASSERT(initialized_);

    ImGuiIO &io = ImGui::GetIO();

    // it may be override by platform specific callbacks, so we need to set it every frame
    io.DisplaySize = ImVec2(static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().width),
                            static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().height));

    ImGui_ImplVulkan_NewFrame();
}

void VulkanUiHandler::Render()
{
    auto &io = ImGui::GetIO();

    // it has been set in UiManager::Render()
    auto *draw_data = reinterpret_cast<ImDrawData *>(io.UserData);

    if (draw_data->CmdListsCount == 0 || draw_data->CmdLists[0]->CmdBuffer.empty())
    {
        return;
    }

    if (draw_data->CmdLists[0]->CmdBuffer[0].TextureId != io.Fonts->TexID)
    {
        return;
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, context->GetCurrentCommandBuffer());
}

VulkanUiHandler::~VulkanUiHandler()
{
    ImGui_ImplVulkan_Shutdown();
    vkDestroyDescriptorPool(context->GetDevice(), descriptor_pool_, nullptr);
    is_valid_ = false;
}

void VulkanUiHandler::CreateDescriptorPool()
{
    ASSERT(!descriptor_pool_);

    std::vector<VkDescriptorPoolSize> pool_sizes = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1},
    };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<unsigned>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = static_cast<unsigned>(pool_sizes.size());

    pool_info.flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    CHECK_VK_ERROR(vkCreateDescriptorPool(context->GetDevice(), &pool_info, nullptr, &descriptor_pool_));
}
} // namespace sparkle

#endif
