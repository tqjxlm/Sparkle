#if FRAMEWORK_APPLE

#include "MetalResourceArray.h"

#include "MetalBuffer.h"
#include "MetalContext.h"
#include "MetalImage.h"

namespace sparkle
{

void MetalResourceArray::Setup()
{
    ASSERT(frame_copies_.empty());

    auto num_resources = GetArraySize();

    size_t entry_size = 0;
    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer:
        entry_size = sizeof(uint64_t);
        break;
    case RHIShaderResourceReflection::ResourceType::Texture2D:
        entry_size = sizeof(MTLResourceID);
        break;
    default:
        UnImplemented(type_);
    }

    frame_copies_.resize(context->GetRHI()->GetMaxFramesInFlight());
    for (auto &copy : frame_copies_)
    {
        copy.argument_buffer.buffer = [context->GetDevice() newBufferWithLength:entry_size * num_resources
                                                                        options:MTLResourceStorageModeShared];
        for (auto i = 0u; i < num_resources; i++)
        {
            if (resources_[i])
            {
                copy.dirty_indices.push_back(i);
            }
        }
    }
}

void MetalResourceArray::OnResourceUpdate()
{
    RHIResourceArray::OnResourceUpdate();

    // every frame copy has to catch up on the change before it is bound
    for (auto &copy : frame_copies_)
    {
        copy.dirty_indices.push_back(dirty_resource_indices_.back());
    }
    FinishUpdate();
}

void MetalResourceArray::UpdateResources(FrameCopy &copy)
{
    auto &argument_buffer = copy.argument_buffer;

    switch (type_)
    {
    case RHIShaderResourceReflection::ResourceType::UniformBuffer:
    case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
        auto *buffer_data = (uint64_t *)argument_buffer.buffer.contents;

        for (auto i : copy.dirty_indices)
        {
            const auto &sub_resource = resources_[i];
            if (sub_resource)
            {
                buffer_data[i] = RHICast<MetalBuffer>(sub_resource)->GetResource().gpuAddress;
            }
        }

        argument_buffer.resources.clear();
        for (const auto &sub_resource : resources_)
        {
            if (sub_resource)
            {
                argument_buffer.resources.emplace_back(RHICast<MetalBuffer>(sub_resource)->GetResource());
            }
        }
        break;
    }
    case RHIShaderResourceReflection::ResourceType::Texture2D: {
        auto *buffer_data = (MTLResourceID *)argument_buffer.buffer.contents;

        for (auto i : copy.dirty_indices)
        {
            const auto &sub_resource = resources_[i];
            if (sub_resource)
            {
                buffer_data[i] = RHICast<MetalImageView>(sub_resource)->GetResource().gpuResourceID;
            }
        }

        argument_buffer.resources.clear();
        for (const auto &sub_resource : resources_)
        {
            if (sub_resource)
            {
                argument_buffer.resources.emplace_back(RHICast<MetalImageView>(sub_resource)->GetResource());
            }
        }
        break;
    }
    default:
        UnImplemented(type_);
    }

    copy.dirty_indices.clear();
}

void MetalResourceArray::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point)
{
    if (frame_copies_.empty())
    {
        Setup();
    }

    // the copy of this frame slot is idle: the frame throttle guarantees the GPU finished the frame that used it
    auto &copy = frame_copies_[context->GetRHI()->GetFrameIndex()];
    if (!copy.dirty_indices.empty())
    {
        UpdateResources(copy);
    }
    auto &argument_buffer = copy.argument_buffer;

    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexBuffer:argument_buffer.buffer offset:0 atIndex:binding_point];
        [render_encoder useResources:argument_buffer.resources.data()
                               count:argument_buffer.resources.size()
                               usage:MTLResourceUsageRead
                              stages:MTLRenderStageVertex];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentBuffer:argument_buffer.buffer offset:0 atIndex:binding_point];
        [render_encoder useResources:argument_buffer.resources.data()
                               count:argument_buffer.resources.size()
                               usage:MTLResourceUsageRead
                              stages:MTLRenderStageFragment];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setBuffer:argument_buffer.buffer offset:0 atIndex:binding_point];
        [compute_encoder useResources:argument_buffer.resources.data()
                                count:argument_buffer.resources.size()
                                usage:MTLResourceUsageRead];
        break;
    default:
        UnImplemented(stage);
        break;
    }
}
} // namespace sparkle

#endif
