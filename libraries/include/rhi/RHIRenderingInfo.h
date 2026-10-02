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
    DontCare,
    Load,
    Clear,
};

enum class RHIStoreOp : uint8_t
{
    DontCare,
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
    // a bit per color slot whose write mask is 0
    uint8_t unwritten_color_slots = 0;
    // depth is neither tested nor written
    bool depth_unused = false;

    bool operator==(const RHIAttachmentSignature &) const = default;
};

struct RHIColorAttachment
{
    // null marks an unused slot
    RHIImage *image = nullptr;
    unsigned mip_level = 0;
    unsigned array_layer = 0;
    // ColorOutput, or LocalRead when draws of the rendering read it pixel-locally
    RHIImageLayout layout = RHIImageLayout::ColorOutput;
    RHILoadOp load_op = RHILoadOp::DontCare;
    RHIStoreOp store_op = RHIStoreOp::Store;
    Vector4 clear_color{0, 0, 0, 1};
};

struct RHIDepthAttachment
{
    RHIImage *image = nullptr;
    unsigned mip_level = 0;
    unsigned array_layer = 0;
    RHILoadOp load_op = RHILoadOp::DontCare;
    RHIStoreOp store_op = RHIStoreOp::DontCare;
    float clear_depth = 1.f;
};

// one render pass instance, rendering into color attachments in their layouts and a depth attachment in the depth
// attachment layout
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
