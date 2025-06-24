#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{

class MetalSampler : public RHISampler
{
public:
    MetalSampler(const SamplerAttribute &attribute, const std::string &name);

    [[nodiscard]] id<MTLSamplerState> GetResource() const
    {
        return sampler_;
    }

    void Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const;

private:
    id<MTLSamplerState> sampler_;
};

class MetalImage : public RHIImage
{
public:
    MetalImage(const Attribute &attributes, const std::string &name);

    MetalImage(const Attribute &attributes, id<MTLTexture> texture, const std::string &name);

    void Transition(const TransitionRequest &) override
    {
    }

    void Upload(const uint8_t *data) override;

    void UploadFaces(std::array<const uint8_t *, 6> data) override;

    void CopyToImage(const RHIImage *image) const override;

    void GenerateMips() override;

    void CopyToBuffer(const RHIBuffer *buffer) const override;

    void BlitToImage(const RHIImage *image, RHISampler::FilteringMethod filter) const override;

    [[nodiscard]] id<MTLTexture> GetResource() const
    {
        return texture_;
    }

    void SetImage(id<MTLTexture> texture);

private:
    void CreateSamplerIfNeeded();

    id<MTLTexture> texture_;
};

class MetalImageView : public RHIImageView
{
public:
    MetalImageView(Attribute attribute, RHIImage *image);

    [[nodiscard]] id<MTLTexture> GetView() const
    {
        return view_;
    }

    void Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const;

    [[nodiscard]] id<MTLTexture> GetResource() const
    {
        return view_;
    }

private:
    id<MTLTexture> view_;
};

inline MTLPixelFormat GetMetalPixelFormat(PixelFormat format)

{
    switch (format)
    {
    case PixelFormat::B8G8R8A8_SRGB:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case PixelFormat::B8G8R8A8_UNORM:
        return MTLPixelFormatBGRA8Unorm;
    case PixelFormat::R8G8B8A8_SRGB:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    case PixelFormat::R8G8B8A8_UNORM:
        return MTLPixelFormatRGBA8Unorm;
#if FRAMEWORK_MACOS
    case PixelFormat::D24_S8:
        return MTLPixelFormatDepth24Unorm_Stencil8;
#endif
    case PixelFormat::D32:
        return MTLPixelFormatDepth32Float;
    case PixelFormat::RGBAFloat:
        return MTLPixelFormatRGBA32Float;
    case PixelFormat::RGBAFloat16:
        return MTLPixelFormatRGBA16Float;
    case PixelFormat::R10G10B10A2_UNORM:
        return MTLPixelFormatRGB10A2Unorm;
    case PixelFormat::R32_UINT:
        return MTLPixelFormatR32Uint;
    case PixelFormat::R32_FLOAT:
        return MTLPixelFormatR32Float;
    case PixelFormat::RGBAUInt32:
        return MTLPixelFormatRGBA32Uint;
    default:
        UnImplemented(format);
    }
    return MTLPixelFormatInvalid;
}
} // namespace sparkle

#endif
