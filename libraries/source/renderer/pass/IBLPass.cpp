#include "renderer/pass/IBLPass.h"

#include "renderer/pass/ClearTexturePass.h" // IWYU pragma: keep
#include "rhi/RHI.h"

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
    ibl_image_ = CreateIBLMap(false, true);

    rhi_->BeginCommandBuffer();

    cooked_ibl_image->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::Transfer});

    ibl_image_->Transition({.target_layout = RHIImageLayout::TransferDst,
                            .after_stage = RHIPipelineStage::Top,
                            .before_stage = RHIPipelineStage::Transfer});

    cooked_ibl_image->BlitToImage(ibl_image_, RHISampler::FilteringMethod::Nearest);

    rhi_->SubmitCommandBuffer();

    is_ready_ = true;

    if (artifact_ready_callback_)
    {
        artifact_ready_callback_(ibl_image_->ReadToMemory(rhi_));
    }
}

void IBLPass::PrepareForCooking()
{
    ASSERT(!is_ready_ && !ibl_image_);
    ibl_image_ = CreateIBLMap(true, true);
}

bool IBLPass::ApplyArtifact(const std::vector<char> &payload)
{
    ASSERT(!is_ready_);

    // A cache miss may have left ibl_image_ in the cooking format; artifacts use the
    // compact resource layout.
    ibl_image_ = CreateIBLMap(false, false);

    if (payload.size() != ibl_image_->GetStorageSize())
    {
        ibl_image_ = nullptr;
        return false;
    }

    ibl_image_->Upload(reinterpret_cast<const uint8_t *>(payload.data()));

    is_ready_ = true;
    return true;
}

void IBLPass::Complete()
{
    rhi_->EnqueueEndOfFrameTasks([this]() { Finalize(); });
}
} // namespace sparkle
