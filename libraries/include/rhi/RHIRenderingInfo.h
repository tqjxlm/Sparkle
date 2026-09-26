#pragma once

#include "core/math/Types.h"
#include "rhi/RHIImage.h"

#include <array>
#include <cstdint>

namespace sparkle
{
constexpr uint8_t MaxNumColorAttachments = 8;

enum class RHILoadOp : uint8_t
{
    None,
    Load,
    Clear,
};

enum class RHIStoreOp : uint8_t
{
    None,
    Store,
};

// what a graphics pipeline is compiled against. a color slot is the fragment output location it receives.
struct RHIAttachmentSignature
{
    // PixelFormat::Count marks an unused slot
    std::array<PixelFormat, MaxNumColorAttachments> color_formats = [] {
        std::array<PixelFormat, MaxNumColorAttachments> formats;
        formats.fill(PixelFormat::Count);
        return formats;
    }();
    PixelFormat depth_format = PixelFormat::Count;
    uint8_t samples = 1;

    bool operator==(const RHIAttachmentSignature &) const = default;
};

struct RHIColorAttachment
{
    // null marks an unused slot
    RHIImage *image = nullptr;
    // the single-sample image a multisampled `image` resolves into. mip_level and array_layer then address this image,
    // and the multisampled image has a single subresource.
    RHIImage *resolve_image = nullptr;
    unsigned mip_level = 0;
    unsigned array_layer = 0;
    RHILoadOp load_op = RHILoadOp::None;
    RHIStoreOp store_op = RHIStoreOp::Store;
    Vector4 clear_color{0, 0, 0, 1};
};

struct RHIDepthAttachment
{
    RHIImage *image = nullptr;
    unsigned mip_level = 0;
    unsigned array_layer = 0;
    RHILoadOp load_op = RHILoadOp::None;
    RHIStoreOp store_op = RHIStoreOp::None;
    float clear_depth = 1.f;
};

// one render pass instance, rendering into attachments in the color and depth attachment layouts
struct RHIRenderingInfo
{
    std::array<RHIColorAttachment, MaxNumColorAttachments> color_attachments;
    RHIDepthAttachment depth_attachment;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t samples = 1;

    [[nodiscard]] RHIAttachmentSignature GetSignature() const
    {
        RHIAttachmentSignature signature;
        for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
        {
            if (const auto *image = color_attachments[slot].image)
            {
                signature.color_formats[slot] = image->GetAttributes().format;
            }
        }
        if (depth_attachment.image)
        {
            signature.depth_format = depth_attachment.image->GetAttributes().format;
        }
        signature.samples = samples;
        return signature;
    }
};
} // namespace sparkle
