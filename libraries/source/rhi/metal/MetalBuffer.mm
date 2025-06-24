#if FRAMEWORK_APPLE

#include "MetalBuffer.h"

#include "MetalContext.h"
#include "MetalImage.h"

namespace sparkle
{
static MTLResourceOptions GetMetalResourceOptions(RHIMemoryProperty memory_property)
{
    MTLResourceOptions option = 0;
    if (memory_property & RHIMemoryProperty::DeviceLocal)
    {
        option |= MTLResourceStorageModePrivate;
    }
    else
    {
        // on apple silicon, it is always shared memory unless specifically specified.
        option |= MTLResourceStorageModeShared;
    }

    return option;
}

void MetalBuffer::CopyToImage(const RHIImage *image) const
{
    auto mtl_image = RHICast<MetalImage>(image)->GetResource();

    auto command_buffer = context->GetCurrentCommandBuffer();
    auto command_encoder = [command_buffer blitCommandEncoder];

    uint32_t copied_bytes = 0;
    auto src_offset = GetOffset(context->GetRHI()->GetFrameIndex());

    unsigned num_layers = image->GetAttributes().type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    for (auto layer = 0u; layer < num_layers; layer++)
    {
        for (auto mip_level = 0u; mip_level < image->GetAttributes().mip_levels; mip_level++)
        {
            [command_encoder copyFromBuffer:GetResource()
                               sourceOffset:(src_offset + copied_bytes)
                          sourceBytesPerRow:image->GetBytesPerRow(mip_level)
                        sourceBytesPerImage:0
                                 sourceSize:{image->GetWidth(mip_level), image->GetHeight(mip_level), 1}
                                  toTexture:mtl_image
                           destinationSlice:0
                           destinationLevel:mip_level
                          destinationOrigin:{0, 0, 0}];

            copied_bytes += image->GetStorageSize(mip_level);
        }
    }

    [command_encoder endEncoding];
}

MetalBuffer::MetalBuffer(const RHIBuffer::Attribute &attribute, const std::string &name) : RHIBuffer(attribute, name)
{
    if (attribute_.is_dynamic)
    {
        // we will not create a real buffer in this case since it will be re-generate every frame
        dynamic_allocation_ = context->GetRHI()->GetBufferManager()->SubAllocateDynamicBuffer(attribute_);
    }
    else
    {
        const auto &mem_properties = GetMemoryProperty();
        storage_option_ = GetMetalStorageMode(mem_properties);
        buffer_ = [context->GetDevice() newBufferWithLength:GetSize() options:GetMetalResourceOptions(mem_properties)];

        SetDebugInfo(buffer_, GetName());

        bool always_map = GetMemoryProperty() & RHIMemoryProperty::AlwaysMap;
        if (always_map)
        {
            mapped_address_ = reinterpret_cast<uint8_t *>(buffer_.contents);
        }
    }
}

void MetalBuffer::CopyToBuffer(const RHIBuffer *buffer) const
{
    // copy to dynamic buffer is not supported for now
    ASSERT(!buffer->IsDynamic());

    const auto *dst_buffer = RHICast<MetalBuffer>(buffer);
    auto command_buffer = context->GetCurrentCommandBuffer();
    auto command_encoder = [command_buffer blitCommandEncoder];

    auto src_offset = GetOffset(context->GetRHI()->GetFrameIndex());

    [command_encoder copyFromBuffer:GetResource()
                       sourceOffset:src_offset
                           toBuffer:dst_buffer->GetResource()
                  destinationOffset:dst_buffer->GetOffset(UINT_MAX)
                               size:GetSize()];

    [command_encoder endEncoding];
}

id<MTLBuffer> MetalBuffer::GetResource() const
{
    if (IsDynamic())
    {
        auto *actual_buffer = RHICast<MetalBuffer>(dynamic_allocation_.GetBuffer());
        return actual_buffer->GetResource();
    }
    return buffer_;
}

void MetalBuffer::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const
{
    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;
    auto frame_index = context->GetRHI()->GetFrameIndex();

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexBuffer:GetResource() offset:GetOffset(frame_index) atIndex:binding_point];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentBuffer:GetResource() offset:GetOffset(frame_index) atIndex:binding_point];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setBuffer:GetResource() offset:GetOffset(frame_index) atIndex:binding_point];
        break;
    default:
        UnImplemented(stage);
        break;
    }
}
} // namespace sparkle

#endif
