#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"

namespace sparkle
{
class IBLPass : public PipelinePass
{
public:
    IBLPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &env_map);

    ~IBLPass() override;

    [[nodiscard]] RHIResourceRef<RHIImage> GetResource() const
    {
        if (!is_ready_)
        {
            return nullptr;
        }
        return ibl_image_;
    }

    [[nodiscard]] bool IsReady() const
    {
        return is_ready_;
    }

    virtual void CookOnTheFly(const RenderConfig &config, unsigned samples_per_dispatch) = 0;

protected:
    void Complete();

    void TryLoad();

    virtual RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write) = 0;

    [[nodiscard]] virtual std::string GetCachePath() const = 0;

    RHIResourceRef<RHIImage> ibl_image_;

    RHIResourceRef<RHIImage> env_map_;

    RHIResourceRef<RHIRenderTarget> clear_target_;

    std::unique_ptr<class ClearTexturePass> clear_pass_;

    RHIResourceRef<RHIShader> compute_shader_;

    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    RHIResourceRef<RHIBuffer> cs_ub_;

    unsigned sample_count_ = 0;
    unsigned target_sample_count_ = 2048;

    static constexpr uint8_t MipLevelCount = 5;

private:
    void Save();

    bool is_ready_ = false;
};
} // namespace sparkle
