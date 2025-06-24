#if FRAMEWORK_APPLE

#include "MetalImage.h"

#include "MetalBuffer.h"
#include "MetalContext.h"

#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

namespace sparkle
{
static MTLTextureUsage GetMetalTextureUsage(RHIImage::ImageUsage usage)
{
    NSUInteger metal_usage = MTLTextureUsageUnknown;

    if (usage & RHIImage::ImageUsage::ColorAttachment || usage & RHIImage::ImageUsage::DepthStencilAttachment ||
        usage & RHIImage::ImageUsage::TransientAttachment)
    {
        metal_usage |= MTLTextureUsageRenderTarget;
    }

    if (usage & RHIImage::ImageUsage::Texture || usage & RHIImage::ImageUsage::SRV || usage & RHIImage::ImageUsage::UAV)
    {
        metal_usage |= MTLTextureUsageShaderRead;
    }

    if (usage & RHIImage::ImageUsage::UAV)
    {
        metal_usage |= MTLTextureUsageShaderWrite;
    }

    return metal_usage;
}

static MTLSamplerAddressMode GetMetalSamplerAddressMode(RHISampler::SamplerAddressMode address_mode)
{
    switch (address_mode)
    {
    case RHISampler::SamplerAddressMode::Repeat:
        return MTLSamplerAddressModeRepeat;
    case RHISampler::SamplerAddressMode::RepeatMirror:
        return MTLSamplerAddressModeMirrorRepeat;
    case RHISampler::SamplerAddressMode::ClampToEdge:
        return MTLSamplerAddressModeClampToEdge;
    case RHISampler::SamplerAddressMode::ClampToBorder:
        return MTLSamplerAddressModeClampToBorderColor;
    case RHISampler::SamplerAddressMode::Count:
        UnImplemented(address_mode);
    }
    return MTLSamplerAddressModeClampToEdge;
}

static MTLSamplerMipFilter GetMetalMipFilter(RHISampler::FilteringMethod filter_method)
{
    switch (filter_method)
    {
    case RHISampler::FilteringMethod::Nearest:
        return MTLSamplerMipFilterNearest;
    case RHISampler::FilteringMethod::Linear:
        return MTLSamplerMipFilterLinear;
    case RHISampler::FilteringMethod::Count:
        return MTLSamplerMipFilterNotMipmapped;
    }
}

static MTLSamplerMinMagFilter GetMetalMinMagFilter(RHISampler::FilteringMethod filter_method)
{
    switch (filter_method)
    {
    case RHISampler::FilteringMethod::Nearest:
        return MTLSamplerMinMagFilterNearest;
    case RHISampler::FilteringMethod::Linear:
        return MTLSamplerMinMagFilterLinear;
    case RHISampler::FilteringMethod::Count:
        ASSERT(false);
        return MTLSamplerMinMagFilterLinear;
    }
}

static MTLTextureType GetMetalTextureType(RHIImageView::ImageViewType type)
{
    switch (type)
    {
    case RHIImageView::ImageViewType::Image2D:
        return MTLTextureType2D;
    case RHIImageView::ImageViewType::Image2DCube:
        return MTLTextureTypeCube;
    case RHIImageView::ImageViewType::Image2DArray:
        return MTLTextureType2DArray;
    }
}

static MTLSamplerBorderColor GetMetalSamplerBorderColor(RHISampler::BorderColor border_color)
{
    switch (border_color)
    {
    case RHISampler::BorderColor::IntTransparentBlack:
    case RHISampler::BorderColor::FloatTransparentBlack:
        return MTLSamplerBorderColorTransparentBlack;
    case RHISampler::BorderColor::IntOpaqueBlack:
    case RHISampler::BorderColor::FloatOpaqueBlack:
        return MTLSamplerBorderColorOpaqueBlack;
    case RHISampler::BorderColor::IntOpaqueWhite:
    case RHISampler::BorderColor::FloatOpaqueWhite:
        return MTLSamplerBorderColorOpaqueWhite;
    case RHISampler::BorderColor::Count:
        UnImplemented(border_color);
    }
    return MTLSamplerBorderColorTransparentBlack;
}

MetalSampler::MetalSampler(const SamplerAttribute &attribute, const std::string &name) : RHISampler(attribute, name)
{
    MTLSamplerDescriptor *sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.rAddressMode = GetMetalSamplerAddressMode(attribute_.address_mode);
    sampler_descriptor.sAddressMode = GetMetalSamplerAddressMode(attribute_.address_mode);
    sampler_descriptor.tAddressMode = GetMetalSamplerAddressMode(attribute_.address_mode);
    if (attribute_.address_mode == RHISampler::SamplerAddressMode::ClampToBorder)
    {
        sampler_descriptor.borderColor = GetMetalSamplerBorderColor(attribute_.border_color);
    }
    sampler_descriptor.supportArgumentBuffers = true;
    sampler_descriptor.minFilter = GetMetalMinMagFilter(attribute_.filtering_method_min);
    sampler_descriptor.magFilter = GetMetalMinMagFilter(attribute_.filtering_method_mag);
    sampler_descriptor.mipFilter = GetMetalMipFilter(attribute_.filtering_method_mipmap);

    SetDebugInfo(sampler_descriptor, GetName());

    sampler_ = [context->GetDevice() newSamplerStateWithDescriptor:sampler_descriptor];
}

MetalImage::MetalImage(const Attribute &attributes, const std::string &name) : RHIImage(attributes, name)
{
    MTLTextureDescriptor *texture_descriptor = [[MTLTextureDescriptor alloc] init];

    texture_descriptor.pixelFormat = GetMetalPixelFormat(attributes.format);
    texture_descriptor.width = attributes.width;
    texture_descriptor.height = attributes.height;
    texture_descriptor.mipmapLevelCount = attributes.mip_levels;
    texture_descriptor.usage = GetMetalTextureUsage(attributes.usages);
    texture_descriptor.storageMode = GetMetalStorageMode(attributes.memory_properties);
    texture_descriptor.arrayLength = 1;

    if (attributes.type == RHIImage::ImageType::Image2DCube)
    {
        texture_descriptor.textureType = MTLTextureTypeCube;
    }
    else
    {
        texture_descriptor.textureType = MTLTextureType2D;
    }

    texture_ = [context->GetDevice() newTextureWithDescriptor:texture_descriptor];

    SetDebugInfo(texture_, GetName());

    ASSERT_F(texture_, "Failed to created texture {}", name);

    CreateSamplerIfNeeded();
}

MetalImage::MetalImage(const Attribute &attributes, id<MTLTexture> texture, const std::string &name)
    : RHIImage(attributes, name)
{
    texture_ = texture;

    CreateSamplerIfNeeded();
}

void MetalImage::Upload(const uint8_t *data)
{
    uint32_t copied_bytes = 0;

    unsigned num_layers = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    for (auto layer = 0u; layer < num_layers; layer++)
    {
        for (int mip_level = 0; mip_level < attributes_.mip_levels; mip_level++)
        {
            MTLRegion region = {{0, 0, 0}, {GetWidth(mip_level), GetHeight(mip_level), 1}};

            [texture_ replaceRegion:region
                        mipmapLevel:mip_level
                              slice:layer
                          withBytes:(data + copied_bytes)
                        bytesPerRow:GetBytesPerRow(mip_level)
                      bytesPerImage:GetStorageSizePerLayer()];

            copied_bytes += GetStorageSize(mip_level);
        }
    }
}

void MetalImage::UploadFaces(std::array<const uint8_t *, 6> data)
{
    ASSERT(attributes_.type == RHIImage::ImageType::Image2DCube);

    for (auto face = 0; face < 6; face++)
    {
        uint32_t copied_bytes = 0;

        for (auto mip_level = 0u; mip_level < attributes_.mip_levels; mip_level++)
        {
            MTLRegion region = {{0, 0, 0}, {GetWidth(mip_level), GetHeight(mip_level), 1}};

            [texture_ replaceRegion:region
                        mipmapLevel:mip_level
                              slice:face
                          withBytes:(data[face] + copied_bytes)
                        bytesPerRow:GetBytesPerRow(mip_level)
                      bytesPerImage:GetStorageSizePerLayer()];

            copied_bytes += GetStorageSize(mip_level);
        }
    }
}

void MetalImage::CopyToImage(const RHIImage *image) const
{
    const auto *dst_image = RHICast<MetalImage>(image);

    auto dst_texture = dst_image->GetResource();

    auto command_buffer = context->GetCurrentCommandBuffer();
    auto command_encoder = [command_buffer blitCommandEncoder];

    [command_encoder copyFromTexture:texture_ toTexture:dst_texture];

    [command_encoder endEncoding];
}

void MetalImage::GenerateMips()
{
    auto command_buffer = context->GetCurrentCommandBuffer();
    auto command_encoder = [command_buffer blitCommandEncoder];

    [command_encoder generateMipmapsForTexture:texture_];

    [command_encoder endEncoding];
}

void MetalImage::CopyToBuffer(const RHIBuffer *buffer) const
{
    // copy to dynamic buffer is not supported for now
    ASSERT(!buffer->IsDynamic());

    const auto *dst_buffer = RHICast<MetalBuffer>(buffer);
    auto dst_offset = dst_buffer->GetOffset(UINT_MAX);

    auto command_buffer = context->GetCurrentCommandBuffer();
    auto command_encoder = [command_buffer blitCommandEncoder];

    uint32_t copied_bytes = 0;

    unsigned num_layers = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    for (auto layer = 0u; layer < num_layers; layer++)
    {
        for (auto mip_level = 0u; mip_level < attributes_.mip_levels; mip_level++)
        {
            [command_encoder copyFromTexture:texture_
                                 sourceSlice:layer
                                 sourceLevel:mip_level
                                sourceOrigin:{0, 0, 0}
                                  sourceSize:{GetWidth(mip_level), GetHeight(mip_level), 1}
                                    toBuffer:dst_buffer->GetResource()
                           destinationOffset:(dst_offset + copied_bytes)
                      destinationBytesPerRow:GetBytesPerRow(mip_level)
                    destinationBytesPerImage:0];

            copied_bytes += GetStorageSize(mip_level);
        }
    }

    [command_encoder endEncoding];
}

void MetalImage::BlitToImage(const RHIImage *image, RHISampler::FilteringMethod) const
{
    const auto *dst_image = RHICast<MetalImage>(image);

    auto dst_texture = dst_image->GetResource();

    auto command_buffer = context->GetCurrentCommandBuffer();

    unsigned num_layers = attributes_.type == RHIImage::ImageType::Image2DCube ? 6 : 1;

    @autoreleasepool
    {
        MPSImageConversion *scale_kernel = [[MPSImageConversion alloc] initWithDevice:context->GetDevice()];

        for (auto layer = 0u; layer < num_layers; layer++)
        {
            for (auto mip_level = 0u; mip_level < attributes_.mip_levels; mip_level++)
            {
                id<MTLTexture> source_view = [texture_ newTextureViewWithPixelFormat:texture_.pixelFormat
                                                                         textureType:MTLTextureType2D
                                                                              levels:NSMakeRange(mip_level, 1)
                                                                              slices:NSMakeRange(layer, 1)];

                id<MTLTexture> dest_view = [dst_texture newTextureViewWithPixelFormat:dst_texture.pixelFormat
                                                                          textureType:MTLTextureType2D
                                                                               levels:NSMakeRange(mip_level, 1)
                                                                               slices:NSMakeRange(layer, 1)];

                [scale_kernel encodeToCommandBuffer:command_buffer
                                      sourceTexture:source_view
                                 destinationTexture:dest_view];
            }
        }
    }
}

void MetalImage::CreateSamplerIfNeeded()
{
    if (attributes_.usages & RHIImage::ImageUsage::Texture)
    {
        sampler_ = context->GetRHI()->GetSampler(attributes_.sampler);
    }
}

void MetalSampler::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const
{
    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexSamplerState:GetResource() atIndex:binding_point];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentSamplerState:GetResource() atIndex:binding_point];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setSamplerState:GetResource() atIndex:binding_point];
        break;
    default:
        UnImplemented(stage);
        break;
    }
}

void MetalImage::SetImage(id<MTLTexture> texture)
{
    // generally this function should not be called, unless it is a back buffer image
    // TODO(tqjxlm): there should be a more robust way
    texture_ = texture;

    id_dirty_ = true;
}

MetalImageView::MetalImageView(Attribute attribute, RHIImage *image) : RHIImageView(attribute, image)
{
    auto *metal_image = RHICast<MetalImage>(image);

    view_ = [metal_image->GetResource()
        newTextureViewWithPixelFormat:GetMetalPixelFormat(metal_image->GetAttributes().format)
                          textureType:GetMetalTextureType(attribute_.type)
                               levels:NSMakeRange(attribute.base_mip_level, attribute.mip_level_count)
                               slices:NSMakeRange(attribute.base_array_layer, attribute.array_layer_count)];
}

void MetalImageView::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const
{
    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexTexture:GetResource() atIndex:binding_point];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentTexture:GetResource() atIndex:binding_point];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setTexture:GetResource() atIndex:binding_point];
        break;
    default:
        UnImplemented(stage);
        break;
    }
}
} // namespace sparkle

#endif
