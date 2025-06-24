#include "renderer/pass/IBLPass.h"

#include "renderer/pass/ClearTexturePass.h" // IWYU pragma: keep
#include "rhi/RHI.h"

namespace sparkle
{
IBLPass::IBLPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &env_map) : PipelinePass(ctx), env_map_(env_map)
{
}

IBLPass::~IBLPass() = default;

void IBLPass::Save()
{
    ASSERT(is_ready_);

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

    auto file_path = GetCachePath();

    auto written_path = ibl_image_->SaveToFile(file_path, rhi_);

    if (written_path.empty())
    {
        Log(Error, "failed to save ibl cache: {}", file_path);
    }
    else
    {
        Log(Info, "saved ibl cache with size {}kB: {}", ibl_image_->GetStorageSize() / 1024, written_path);
    }
}

void IBLPass::TryLoad()
{
    ibl_image_ = CreateIBLMap(false, false);

    auto file_path = GetCachePath();

    if (ibl_image_->LoadFromFile(file_path))
    {
        Log(Info, "loaded ibl cache from {}", file_path);

        is_ready_ = true;

        return;
    }

    Log(Info, "failed to load valid ibl cache, will generate at runtime to {}", file_path);

    // recreate the image with cooking support
    ibl_image_ = CreateIBLMap(true, true);
}

void IBLPass::Complete()
{
    rhi_->EnqueueEndOfFrameTasks([this]() { Save(); });

    is_ready_ = true;
}
} // namespace sparkle
