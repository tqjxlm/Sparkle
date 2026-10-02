#if ENABLE_VULKAN

#include "VulkanUi.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanPipelineState.h"
#include "VulkanSwapChain.h"

#include <imgui_impl_vulkan.h>

namespace sparkle
{
VulkanUiHandler::VulkanUiHandler()
    : RHIUiHandler("VulkanUiHandler", ImGui_ImplVulkan_UpdateTexture, context->GetRHI()->GetMaxFramesInFlight())
{
#ifdef VK_NO_PROTOTYPES
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

    QueueFamilyIndices const indices = FindQueueFamilies(context->GetPhysicalDevice(), context->GetSurface());

    // no main pipeline yet: Render compiles one for the attachment signature it draws against
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = ApiVersion;
    init_info.Instance = context->GetInstance();
    init_info.PhysicalDevice = context->GetPhysicalDevice();
    init_info.Device = context->GetDevice();
    init_info.QueueFamily = indices.graphicsFamily;
    init_info.Queue = context->GetGraphicsQueue();
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = descriptor_pool_;
    init_info.UseDynamicRendering = true;
    init_info.MinImageCount = 2;
    init_info.ImageCount = context->GetRHI()->GetMaxFramesInFlight();
    init_info.Allocator = VK_NULL_HANDLE;
    init_info.CheckVkResultFn = CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    is_valid_ = true;
}

void VulkanUiHandler::BeginFrame(const RHIRenderingInfo & /*info*/)
{
    ImGui_ImplVulkan_NewFrame();
}

void VulkanUiHandler::Render(RHICommandContext &command_context)
{
    auto &io = ImGui::GetIO();

    // it has been set in UiManager::Render()
    auto *draw_data = reinterpret_cast<ImDrawData *>(io.UserData);

    ProcessTextureRequests(*draw_data);

    if (draw_data->CmdListsCount == 0 || draw_data->CmdLists[0]->CmdBuffer.empty())
    {
        return;
    }

    CompilePipeline(command_context.GetAttachmentSignature());

    auto &vulkan_context = static_cast<VulkanCommandContext &>(command_context);
    ImGui_ImplVulkan_RenderDrawData(draw_data, vulkan_context.GetCommandBuffer());

    // imgui records its pipeline, buffers, descriptor sets and viewport directly
    vulkan_context.ResetCommandState();
}

void VulkanUiHandler::CompilePipeline(const RHIAttachmentSignature &signature)
{
    if (pipeline_signature_ == signature)
    {
        return;
    }

    // ImGui destroys its previous pipeline right away, while earlier frames may still draw with it
    if (pipeline_signature_)
    {
        context->GetRHI()->WaitForDeviceIdle();
    }

    std::array<VkFormat, MaxNumColorAttachments> color_formats;
    ImGui_ImplVulkan_PipelineInfo pipeline_info = {};
    pipeline_info.PipelineRenderingCreateInfo = GetVkPipelineRenderingCreateInfo(signature, color_formats);
    pipeline_info.MSAASamples = GetVkMsaaSampleBit(signature.samples);
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline_info);

    pipeline_signature_ = signature;
}

VulkanUiHandler::~VulkanUiHandler()
{
    ShutdownTextureQueue();
    ImGui_ImplVulkan_Shutdown();
    vkDestroyDescriptorPool(context->GetDevice(), descriptor_pool_, nullptr);
    is_valid_ = false;
}

void VulkanUiHandler::CreateDescriptorPool()
{
    ASSERT(!descriptor_pool_);

    std::vector<VkDescriptorPoolSize> pool_sizes = {
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
    };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<unsigned>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;

    pool_info.flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    CHECK_VK_ERROR(vkCreateDescriptorPool(context->GetDevice(), &pool_info, nullptr, &descriptor_pool_));
}
} // namespace sparkle

#endif
