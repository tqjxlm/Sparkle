#if FRAMEWORK_APPLE

#include "MetalResourceArray.h"

#include "MetalBuffer.h"
#include "MetalContext.h"
#include "MetalImage.h"

namespace sparkle
{

void MetalResourceArray::Setup()
{
    ASSERT(!argument_buffer_.buffer);

    auto num_resources = GetArraySize();

    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
        auto argument_buffer_size = sizeof(uint64_t) * num_resources;

        argument_buffer_.buffer = [context->GetDevice() newBufferWithLength:argument_buffer_size
                                                                    options:MTLResourceStorageModeShared];
        break;
    }
    case RHIShaderResourceReflection::ResourceType::Texture2D: {
        auto argument_buffer_size = sizeof(MTLResourceID) * num_resources;

        argument_buffer_.buffer = [context->GetDevice() newBufferWithLength:argument_buffer_size
                                                                    options:MTLResourceStorageModeShared];
        break;
    }
    default:
        UnImplemented(type_);
    }
}

void MetalResourceArray::UpdateResources()
{
    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
        auto *buffer_data = (uint64_t *)argument_buffer_.buffer.contents;

        for (auto i : dirty_resource_indices_)
        {
            const auto &sub_resource = resources_[i];
            if (!sub_resource)
            {
                continue;
            }
            auto *rhi_buffer = RHICast<MetalBuffer>(sub_resource);
            buffer_data[i] = rhi_buffer->GetResource().gpuAddress;
            argument_buffer_.resources.emplace_back(rhi_buffer->GetResource());
        }
        break;
    }
    case RHIShaderResourceReflection::ResourceType::Texture2D: {
        auto *buffer_data = (MTLResourceID *)argument_buffer_.buffer.contents;

        for (auto i : dirty_resource_indices_)
        {
            const auto &sub_resource = resources_[i];
            if (!sub_resource)
            {
                continue;
            }

            auto *rhi_image = RHICast<MetalImageView>(sub_resource);
            buffer_data[i] = rhi_image->GetResource().gpuResourceID;
            argument_buffer_.resources.emplace_back(rhi_image->GetResource());
        }
        break;
    }
    default:
        UnImplemented(type_);
    }
}

void MetalResourceArray::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point)
{
    if (!argument_buffer_.buffer)
    {
        Setup();
    }

    if (!dirty_resource_indices_.empty())
    {
        UpdateResources();
        FinishUpdate();
    }

    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexBuffer:argument_buffer_.buffer offset:0 atIndex:binding_point];
        [render_encoder useResources:argument_buffer_.resources.data()
                               count:argument_buffer_.resources.size()
                               usage:MTLResourceUsageRead
                              stages:MTLRenderStageVertex];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentBuffer:argument_buffer_.buffer offset:0 atIndex:binding_point];
        [render_encoder useResources:argument_buffer_.resources.data()
                               count:argument_buffer_.resources.size()
                               usage:MTLResourceUsageRead
                              stages:MTLRenderStageFragment];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setBuffer:argument_buffer_.buffer offset:0 atIndex:binding_point];
        [compute_encoder useResources:argument_buffer_.resources.data()
                                count:argument_buffer_.resources.size()
                                usage:MTLResourceUsageRead];
        break;
    default:
        UnImplemented(stage);
        break;
    }

    //[argument_buffer.buffer didModifyRange:NSMakeRange(0, argument_buffer.buffer.length)];
}
} // namespace sparkle

#endif
