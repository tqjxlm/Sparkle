#include "renderer/pass/IBLPass.h"

#include "io/TextureCompression.h"
#include "renderer/pass/ClearTexturePass.h" // IWYU pragma: keep
#include "rhi/RHI.h"

#include <cstring>

namespace sparkle
{
IBLPass::IBLPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &env_map) : PipelinePass(ctx), env_map_(env_map)
{
}

IBLPass::~IBLPass() = default;

void IBLPass::Finalize()
{
    ASSERT(!is_ready_);

    auto cooked_ibl_image = ibl_image_;
    // allow_write must stay on: MoltenVK's converting blit into a non-UAV destination
    // leaves regions of the readback image unwritten (garbage fp16 texels)
    auto fp16_image = CreateIBLMap(false, true, PixelFormat::RGBAFloat16);

    rhi_->BeginCommandBuffer();

    cooked_ibl_image->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::Transfer});

    fp16_image->Transition({.target_layout = RHIImageLayout::TransferDst,
                            .after_stage = RHIPipelineStage::Top,
                            .before_stage = RHIPipelineStage::Transfer});

    cooked_ibl_image->BlitToImage(fp16_image, RHISampler::FilteringMethod::Nearest);

    rhi_->SubmitCommandBuffer();

    ibl_image_ = fp16_image;
    is_ready_ = true;

    if (!artifact_ready_callback_)
    {
        return;
    }

    // a runtime cook delivers the fp16 master; family transcodes are build-time jobs, so no
    // block encoder ever runs on the render thread
    auto fp16_bytes = ibl_image_->ReadToMemory(rhi_);
    artifact_ready_callback_(TextureCompression::WrapFp16Payload(
        reinterpret_cast<const uint8_t *>(fp16_bytes.data()), fp16_bytes.size(), ibl_image_->GetWidth(),
        ibl_image_->GetHeight(), ibl_image_->GetAttributes().mip_levels));
}

void IBLPass::PrepareForCooking()
{
    ASSERT(!is_ready_ && !ibl_image_);
    ibl_image_ = CreateIBLMap(true, true, PixelFormat::RGBAFloat16);
}

RHIResourceRef<RHIImage> IBLPass::MakeIblResource(const std::vector<char> &payload)
{
    if (payload.size() < sizeof(TextureCompression::PayloadHeader))
    {
        return nullptr;
    }

    TextureCompression::PayloadHeader header;
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.format >= static_cast<uint32_t>(PixelFormat::Count))
    {
        return nullptr;
    }

    const auto format = static_cast<PixelFormat>(header.format);
    if (!IsHDRFormat(format))
    {
        return nullptr;
    }

    if (rhi_->SupportsSampledFormat(format))
    {
        auto image = CreateIBLMap(false, false, format);
        if (payload.size() != sizeof(header) + image->GetStorageSize())
        {
            return nullptr;
        }
        image->Upload(reinterpret_cast<const uint8_t *>(payload.data()) + sizeof(header));
        return image;
    }

    const auto fp16_bytes = TextureCompression::DecodeHdrCube(payload);
    auto image = CreateIBLMap(false, false, PixelFormat::RGBAFloat16);
    if (fp16_bytes.size() != image->GetStorageSize())
    {
        return nullptr;
    }
    image->Upload(fp16_bytes.data());
    return image;
}

bool IBLPass::ApplyArtifact(const std::vector<char> &payload)
{
    ASSERT(!is_ready_);

    ibl_image_ = MakeIblResource(payload);
    if (!ibl_image_)
    {
        return false;
    }

    is_ready_ = true;
    return true;
}

void IBLPass::Complete()
{
    rhi_->EnqueueEndOfFrameTasks([this]() { Finalize(); });
}
} // namespace sparkle
