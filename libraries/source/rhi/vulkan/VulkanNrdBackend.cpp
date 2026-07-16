#if ENABLE_VULKAN

#include "VulkanNrdBackend.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"

#include "core/Logger.h"
#include "core/math/Utilities.h"

#include <NRD.h>
#include <spirv_reflect.h>

namespace sparkle
{
namespace
{
VkFormat NrdFormatToVulkan(uint32_t nrd_format)
{
    switch (static_cast<nrd::Format>(nrd_format))
    {
    case nrd::Format::R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case nrd::Format::R8_UINT:
        return VK_FORMAT_R8_UINT;
    case nrd::Format::RG8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case nrd::Format::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::R16_UNORM:
        return VK_FORMAT_R16_UNORM;
    case nrd::Format::R16_UINT:
        return VK_FORMAT_R16_UINT;
    case nrd::Format::R16_SFLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case nrd::Format::RG16_SFLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case nrd::Format::RGBA16_UNORM:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_SFLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case nrd::Format::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case nrd::Format::R32_SFLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case nrd::Format::RG32_SFLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case nrd::Format::RGBA32_SFLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case nrd::Format::R11_G11_B10_UFLOAT:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:
        return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default:
        ASSERT_F(false, "NRD: unmapped format {}", nrd_format);
        return VK_FORMAT_UNDEFINED;
    }
}

RHISampler::SamplerAttribute NrdSamplerAttribute(uint32_t nrd_sampler)
{
    const bool linear = static_cast<nrd::Sampler>(nrd_sampler) == nrd::Sampler::LINEAR_CLAMP;
    const auto filter = linear ? RHISampler::FilteringMethod::Linear : RHISampler::FilteringMethod::Nearest;
    return {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
            .filtering_method_min = filter,
            .filtering_method_mag = filter,
            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest,
            .enable_anisotropy = false};
}
} // namespace

VulkanNrdBackend::~VulkanNrdBackend()
{
    vkDeviceWaitIdle(context->GetDevice());

    for (auto &pipeline : pipelines_)
    {
        vkDestroyPipeline(context->GetDevice(), pipeline.pso, nullptr);
        vkDestroyPipelineLayout(context->GetDevice(), pipeline.pipeline_layout, nullptr);
        for (auto &set_layout : pipeline.set_layouts)
        {
            vkDestroyDescriptorSetLayout(context->GetDevice(), set_layout, nullptr);
        }
        vkDestroyShaderModule(context->GetDevice(), pipeline.shader_module, nullptr);
    }

    for (auto &pool : descriptor_pools_)
    {
        vkDestroyDescriptorPool(context->GetDevice(), pool, nullptr);
    }

    for (auto &image : permanent_pool_)
    {
        DestroyPoolImage(image);
    }
    for (auto &image : transient_pool_)
    {
        DestroyPoolImage(image);
    }
}

bool VulkanNrdBackend::AddPipeline(const CookedPipeline &pipeline)
{
    SpvReflectShaderModule reflection;
    if (spvReflectCreateShaderModule(pipeline.shader_source.size(), pipeline.shader_source.data(), &reflection) !=
        SPV_REFLECT_RESULT_SUCCESS)
    {
        Log(Error, "VulkanNrdBackend: SPIR-V reflection failed for '{}'", pipeline.entry_point);
        return false;
    }

    uint32_t binding_count = 0;
    spvReflectEnumerateDescriptorBindings(&reflection, &binding_count, nullptr);
    std::vector<SpvReflectDescriptorBinding *> reflected_bindings(binding_count);
    spvReflectEnumerateDescriptorBindings(&reflection, &binding_count, reflected_bindings.data());

    const auto &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;

    NrdPipeline nrd_pipeline;
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> per_set_bindings;

    auto map_register = [](std::vector<BindingRef> &out, uint32_t reg, const SpvReflectDescriptorBinding *reflected) {
        if (reg >= out.size())
        {
            out.resize(reg + 1);
        }
        out[reg] = {.set = reflected->set, .binding = reflected->binding};
    };

    for (const auto *reflected : reflected_bindings)
    {
        VkDescriptorType type;
        switch (reflected->descriptor_type)
        {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
            map_register(nrd_pipeline.sampler_bindings, reflected->binding - offsets.samplerOffset, reflected);
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            map_register(nrd_pipeline.srv_bindings, reflected->binding - offsets.textureOffset, reflected);
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            map_register(nrd_pipeline.uav_bindings, reflected->binding - offsets.storageTextureAndBufferOffset,
                         reflected);
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            nrd_pipeline.cb_binding = {.set = reflected->set, .binding = reflected->binding};
            break;
        default:
            Log(Error, "VulkanNrdBackend: unexpected descriptor type {} in '{}'",
                static_cast<int>(reflected->descriptor_type), pipeline.entry_point);
            spvReflectDestroyShaderModule(&reflection);
            return false;
        }

        if (reflected->set >= per_set_bindings.size())
        {
            per_set_bindings.resize(reflected->set + 1);
        }
        per_set_bindings[reflected->set].push_back({.binding = reflected->binding,
                                                    .descriptorType = type,
                                                    .descriptorCount = 1,
                                                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .pImmutableSamplers = nullptr});
    }

    spvReflectDestroyShaderModule(&reflection);

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = pipeline.shader_source.size();
    module_info.pCode = reinterpret_cast<const uint32_t *>(pipeline.shader_source.data());
    CHECK_VK_ERROR(vkCreateShaderModule(context->GetDevice(), &module_info, nullptr, &nrd_pipeline.shader_module));

    nrd_pipeline.set_layouts.resize(per_set_bindings.size());
    for (size_t set = 0; set < per_set_bindings.size(); set++)
    {
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<uint32_t>(per_set_bindings[set].size());
        layout_info.pBindings = per_set_bindings[set].data();
        CHECK_VK_ERROR(
            vkCreateDescriptorSetLayout(context->GetDevice(), &layout_info, nullptr, &nrd_pipeline.set_layouts[set]));
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = static_cast<uint32_t>(nrd_pipeline.set_layouts.size());
    pipeline_layout_info.pSetLayouts = nrd_pipeline.set_layouts.data();
    CHECK_VK_ERROR(
        vkCreatePipelineLayout(context->GetDevice(), &pipeline_layout_info, nullptr, &nrd_pipeline.pipeline_layout));

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = nrd_pipeline.shader_module;
    pipeline_info.stage.pName = pipeline.entry_point.c_str();
    pipeline_info.layout = nrd_pipeline.pipeline_layout;
    CHECK_VK_ERROR(
        vkCreateComputePipelines(context->GetDevice(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &nrd_pipeline.pso));

    Log(Info, "VulkanNrdBackend: [{:2}] SRV={} UAV={} samplers={} sets={}", pipelines_.size(),
        nrd_pipeline.srv_bindings.size(), nrd_pipeline.uav_bindings.size(), nrd_pipeline.sampler_bindings.size(),
        nrd_pipeline.set_layouts.size());

    pipelines_.push_back(std::move(nrd_pipeline));
    return true;
}

VulkanNrdBackend::PoolImage VulkanNrdBackend::CreatePoolImage(const PoolTexture &desc, uint32_t width, uint32_t height,
                                                              uint32_t index)
{
    const uint32_t factor = desc.downsample_factor;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = NrdFormatToVulkan(desc.format);
    image_info.extent = {.width = (width + factor - 1) / factor, .height = (height + factor - 1) / factor, .depth = 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;

    PoolImage pool_image;
    CHECK_VK_ERROR(vmaCreateImage(context->GetMemoryAllocator(), &image_info, &allocation_info, &pool_image.image,
                                  &pool_image.allocation, nullptr));

    context->SetDebugInfo(reinterpret_cast<uint64_t>(pool_image.image), VK_OBJECT_TYPE_IMAGE,
                          std::format("NrdPoolTexture_{}", index).c_str());

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = pool_image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .baseMipLevel = 0,
                                  .levelCount = 1,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1};
    CHECK_VK_ERROR(vkCreateImageView(context->GetDevice(), &view_info, nullptr, &pool_image.view));

    return pool_image;
}

void VulkanNrdBackend::DestroyPoolImage(PoolImage &pool_image)
{
    if (pool_image.view)
    {
        vkDestroyImageView(context->GetDevice(), pool_image.view, nullptr);
    }
    if (pool_image.image)
    {
        vmaDestroyImage(context->GetMemoryAllocator(), pool_image.image, pool_image.allocation);
    }
    pool_image = {};
}

void VulkanNrdBackend::AllocateResources(uint32_t width, uint32_t height, const PoolTexture *permanent,
                                         uint32_t permanent_count, const PoolTexture *transient,
                                         uint32_t transient_count, const uint32_t *samplers, uint32_t sampler_count,
                                         uint32_t constant_buffer_size)
{
    permanent_pool_.reserve(permanent_count);
    for (uint32_t i = 0; i < permanent_count; i++)
    {
        permanent_pool_.push_back(CreatePoolImage(permanent[i], width, height, i));
    }

    transient_pool_.reserve(transient_count);
    for (uint32_t i = 0; i < transient_count; i++)
    {
        transient_pool_.push_back(CreatePoolImage(transient[i], width, height, permanent_count + i));
    }

    samplers_.reserve(sampler_count);
    for (uint32_t i = 0; i < sampler_count; i++)
    {
        samplers_.push_back(context->GetRHI()->GetSampler(NrdSamplerAttribute(samplers[i])));
    }

    constant_buffer_data_size_ = constant_buffer_size;
    constant_slot_size_ = utilities::AlignAddress(constant_buffer_size, context->GetMinBufferOffsetAlignment());
    constant_slot_count_ = 256;
    constant_buffer_ = context->GetRHI()->CreateBuffer(
        {.size = static_cast<size_t>(constant_slot_size_) * constant_slot_count_,
         .usages = RHIBuffer::BufferUsage::UniformBuffer,
         .mem_properties =
             RHIMemoryProperty::AlwaysMap | RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
         .is_dynamic = false},
        "NrdConstantBuffer");

    const uint32_t frames_in_flight = context->GetRHI()->GetMaxFramesInFlight();
    descriptor_pools_.resize(frames_in_flight);
    const std::array<VkDescriptorPoolSize, 4> pool_sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
                                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
                                                          {VK_DESCRIPTOR_TYPE_SAMPLER, 256},
                                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128}}};
    for (auto &pool : descriptor_pools_)
    {
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 128;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        CHECK_VK_ERROR(vkCreateDescriptorPool(context->GetDevice(), &pool_info, nullptr, &pool));
    }

    Log(Info, "VulkanNrdBackend: allocated pool {}+{} textures, {} samplers, cb {}B, at {}x{}", permanent_count,
        transient_count, sampler_count, constant_buffer_size, width, height);
}

void VulkanNrdBackend::InitializePoolLayouts(VkCommandBuffer command_buffer)
{
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(permanent_pool_.size() + transient_pool_.size());
    for (const auto *pool : {&permanent_pool_, &transient_pool_})
    {
        for (const auto &image : *pool)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = 1,
                                        .baseArrayLayer = 0,
                                        .layerCount = 1};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers.push_back(barrier);
        }
    }

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());

    pool_layouts_initialized_ = true;
}

void VulkanNrdBackend::RunDispatches(const Dispatch *dispatches, uint32_t count)
{
    auto *rhi = context->GetRHI();
    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();

    if (!pool_layouts_initialized_)
    {
        InitializePoolLayouts(command_buffer);
    }

    VkDescriptorPool descriptor_pool = descriptor_pools_[rhi->GetFrameIndex()];
    CHECK_VK_ERROR(vkResetDescriptorPool(context->GetDevice(), descriptor_pool, 0));

    for (uint32_t d = 0; d < count; d++)
    {
        const Dispatch &dispatch = dispatches[d];
        const NrdPipeline &pipeline = pipelines_[dispatch.pipeline_index];

        if (d > 0)
        {
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }

        std::vector<VkDescriptorSet> descriptor_sets(pipeline.set_layouts.size());
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = static_cast<uint32_t>(pipeline.set_layouts.size());
        alloc_info.pSetLayouts = pipeline.set_layouts.data();
        CHECK_VK_ERROR(vkAllocateDescriptorSets(context->GetDevice(), &alloc_info, descriptor_sets.data()));

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorImageInfo> image_infos;
        VkDescriptorBufferInfo buffer_info{};
        writes.reserve(dispatch.resource_count + samplers_.size() + 1);
        image_infos.reserve(dispatch.resource_count + samplers_.size());

        auto write_descriptor = [&](const BindingRef &target, VkDescriptorType type,
                                    const VkDescriptorImageInfo &info) {
            image_infos.push_back(info);
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_sets[target.set];
            write.dstBinding = target.binding;
            write.descriptorCount = 1;
            write.descriptorType = type;
            write.pImageInfo = &image_infos.back();
            writes.push_back(write);
        };

        if (pipeline.cb_binding.set != ~0u && dispatch.constant_size > 0)
        {
            const uint32_t offset = constant_slot_cursor_ * constant_slot_size_;
            constant_slot_cursor_ = (constant_slot_cursor_ + 1) % constant_slot_count_;
            memcpy(constant_buffer_->GetMappedAddress() + offset, dispatch.constant_data, dispatch.constant_size);

            buffer_info = {.buffer = RHICast<VulkanBuffer>(constant_buffer_.get())->GetResource(),
                           .offset = offset,
                           .range = constant_buffer_data_size_};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_sets[pipeline.cb_binding.set];
            write.dstBinding = pipeline.cb_binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &buffer_info;
            writes.push_back(write);
        }

        for (uint32_t s = 0; s < samplers_.size(); s++)
        {
            if (s < pipeline.sampler_bindings.size() && pipeline.sampler_bindings[s].set != ~0u)
            {
                write_descriptor(pipeline.sampler_bindings[s], VK_DESCRIPTOR_TYPE_SAMPLER,
                                 {.sampler = RHICast<VulkanSampler>(samplers_[s].get())->GetSampler(),
                                  .imageView = VK_NULL_HANDLE,
                                  .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED});
            }
        }

        uint32_t srv_register = 0;
        uint32_t uav_register = 0;
        for (uint32_t r = 0; r < dispatch.resource_count; r++)
        {
            const DispatchResource &resource = dispatch.resources[r];

            VkImageView view = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
            switch (resource.source)
            {
            case DispatchResource::Source::PermanentPool:
                view = permanent_pool_[resource.index_in_pool].view;
                break;
            case DispatchResource::Source::TransientPool:
                view = transient_pool_[resource.index_in_pool].view;
                break;
            case DispatchResource::Source::User: {
                resource.user_image->Transition(
                    {.target_layout = resource.is_uav ? RHIImageLayout::StorageWrite : RHIImageLayout::Read,
                     .after_stage = RHIPipelineStage::ComputeShader,
                     .before_stage = RHIPipelineStage::ComputeShader});
                auto user_view = resource.user_image->GetDefaultView(rhi);
                view = RHICast<VulkanImageView>(user_view.get())->GetView();
                layout = resource.is_uav ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                break;
            }
            default:
                break;
            }

            const auto &bindings = resource.is_uav ? pipeline.uav_bindings : pipeline.srv_bindings;
            const uint32_t reg = resource.is_uav ? uav_register++ : srv_register++;
            if (reg < bindings.size() && bindings[reg].set != ~0u)
            {
                write_descriptor(bindings[reg],
                                 resource.is_uav ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                 {.sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = layout});
            }
        }

        vkUpdateDescriptorSets(context->GetDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        context->BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pso);
        context->BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline_layout, 0, descriptor_sets.data(),
                                    static_cast<uint32_t>(descriptor_sets.size()));
        vkCmdDispatch(command_buffer, dispatch.grid_width, dispatch.grid_height, 1);
    }
}
} // namespace sparkle

#endif
