#include "renderer/pass/IBLPass.h"

#include "io/TextureCompression.h"
#include "renderer/pass/ClearTexturePass.h" // IWYU pragma: keep
#include "rhi/RHI.h"

#include <cstring>

namespace sparkle
{
IBLPass::IBLPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &env_map, PixelFormat target_format)
    : PipelinePass(ctx), target_format_(target_format), env_map_(env_map)
{
}

IBLPass::~IBLPass() = default;

void IBLPass::Finalize()
{
    ASSERT(!is_ready_);

    auto cooked_ibl_image = ibl_image_;
    auto fp16_image = CreateIBLMap(false, false, PixelFormat::RGBAFloat16);

    rhi_->BeginCommandBuffer();

    cooked_ibl_image->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::Transfer});

    fp16_image->Transition({.target_layout = RHIImageLayout::TransferDst,
                            .after_stage = RHIPipelineStage::Top,
                            .before_stage = RHIPipelineStage::Transfer});

    cooked_ibl_image->BlitToImage(fp16_image, RHISampler::FilteringMethod::Nearest);

    rhi_->SubmitCommandBuffer();

    // the resident resource stays fp16 for a runtime on-the-fly cook; the compressed resident
    // form is built by ApplyArtifact off the cook lifecycle, where a resource upload is safe
    ibl_image_ = fp16_image;
    is_ready_ = true;

    if (!artifact_ready_callback_)
    {
        return;
    }

    auto fp16_bytes = ibl_image_->ReadToMemory(rhi_);
    if (target_format_ == PixelFormat::RGBAFloat16)
    {
        artifact_ready_callback_(std::move(fp16_bytes));
        return;
    }

    auto payload = TextureCompression::EncodeHdrCube(reinterpret_cast<const uint8_t *>(fp16_bytes.data()),
                                                     ibl_image_->GetWidth(), ibl_image_->GetHeight(),
                                                     ibl_image_->GetAttributes().mip_levels, target_format_);
    artifact_ready_callback_(std::move(payload));
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
    if (static_cast<PixelFormat>(header.format) != target_format_)
    {
        return nullptr;
    }

    if (rhi_->SupportsSampledFormat(target_format_))
    {
        auto image = CreateIBLMap(false, false, target_format_);
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

    if (target_format_ == PixelFormat::RGBAFloat16)
    {
        ibl_image_ = CreateIBLMap(false, false, PixelFormat::RGBAFloat16);
        if (payload.size() != ibl_image_->GetStorageSize())
        {
            ibl_image_ = nullptr;
            return false;
        }
        ibl_image_->Upload(reinterpret_cast<const uint8_t *>(payload.data()));
        is_ready_ = true;
        return true;
    }

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
