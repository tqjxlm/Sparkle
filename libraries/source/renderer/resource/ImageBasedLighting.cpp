#include "renderer/resource/ImageBasedLighting.h"

#include "core/Profiler.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"
#include "core/task/TaskManager.h"
#include "renderer/RenderConfig.h"
#include "renderer/pass/IBLBrdfPass.h"
#include "renderer/pass/IBLDiffusePass.h"
#include "renderer/pass/IBLSpecularPass.h"
#include "renderer/resource/IblBrdfCookJob.h"
#include "renderer/resource/IblEnvCookJobs.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <array>

namespace sparkle
{
namespace
{
template <class Pass, class... Args>
std::unique_ptr<Pass> CreatePass(const RenderConfig &config, bool allow_gpu_cook, const CookArtifactKey &key,
                                 std::function<void()> on_ready, Args &&...args)
{
    auto pass = std::make_unique<Pass>(std::forward<Args>(args)...);

    if (auto payload = CookArtifactStore::Load(key); !payload.empty() && pass->ApplyArtifact(payload))
    {
        return pass;
    }

    Log(Info, "no valid ibl artifact for {}, will cook at runtime", key.source_name);

    if (allow_gpu_cook)
    {
        pass->SetArtifactReadyCallback([key, ready_callback = std::move(on_ready)](std::vector<char> artifact_payload) {
            CookArtifactStore::Save(key, artifact_payload);
            ready_callback();
        });
        pass->InitRenderResources(config);
    }

    return pass;
}
} // namespace

ImageBasedLighting::ImageBasedLighting(const RHIResourceRef<RHIImage> &env_map,
                                       std::shared_ptr<const Image2DCube> env_map_cpu)
    : env_map_(env_map), env_map_cpu_(std::move(env_map_cpu))
{
}

ImageBasedLighting::~ImageBasedLighting()
{
    alive_->store(false);
}

bool ImageBasedLighting::NeedUpdate() const
{
    return (ibl_diffuse_pass_ && !ibl_diffuse_pass_->IsReady()) ||
           (ibl_specular_pass_ && !ibl_specular_pass_->IsReady()) || (ibl_brdf_pass_ && !ibl_brdf_pass_->IsReady());
}

RHIResourceRef<RHIImage> ImageBasedLighting::GetDiffuseMap() const
{
    return ibl_diffuse_pass_ ? ibl_diffuse_pass_->GetResource() : nullptr;
}

RHIResourceRef<RHIImage> ImageBasedLighting::GetSpecularMap() const
{
    return ibl_specular_pass_ ? ibl_specular_pass_->GetResource() : nullptr;
}

RHIResourceRef<RHIImage> ImageBasedLighting::GetBRDFMap() const
{
    return ibl_brdf_pass_ ? ibl_brdf_pass_->GetResource() : nullptr;
}

void ImageBasedLighting::InitRenderResources(RHIContext *ctx, const RenderConfig &config)
{
    ASSERT(env_map_);

    PROFILE_SCOPE_LOG("ImageBasedLighting::InitRenderResources");

    rhi_ = ctx;
    const bool allow_gpu_cook = ctx->HasPhysicalGpu();

    auto brdf_job = std::make_unique<IblBrdfCookJob>();
    auto diffuse_job = std::make_unique<IblDiffuseCookJob>(env_map_cpu_);
    auto specular_job = std::make_unique<IblSpecularCookJob>(env_map_cpu_);

    auto on_ready = [this, alive = alive_]() {
        if (alive->load())
        {
            render_resource_change_event_.Trigger();
        }
    };

    ibl_brdf_pass_ = CreatePass<IBLBrdfPass>(config, allow_gpu_cook, MakeCookArtifactKey(*brdf_job), on_ready, ctx);
    ibl_diffuse_pass_ =
        CreatePass<IBLDiffusePass>(config, allow_gpu_cook, MakeCookArtifactKey(*diffuse_job), on_ready, ctx, env_map_);
    ibl_specular_pass_ = CreatePass<IBLSpecularPass>(config, allow_gpu_cook, MakeCookArtifactKey(*specular_job),
                                                     on_ready, ctx, env_map_);

    if (!allow_gpu_cook)
    {
        RequestCpuCook(std::move(brdf_job), std::move(diffuse_job), std::move(specular_job));
    }
}

void ImageBasedLighting::RequestCpuCook(std::unique_ptr<CookJob> brdf_job, std::unique_ptr<CookJob> diffuse_job,
                                        std::unique_ptr<CookJob> specular_job)
{
    cpu_cook_pending_ = true;

    auto request = [this](IBLPass *pass, std::unique_ptr<CookJob> job) {
        if (pass->IsReady())
        {
            return;
        }

        cook_handles_.push_back(Cooker::Request(std::move(job), [this, pass, alive = alive_](CookResult result) {
            if (!result.HasPayload())
            {
                Log(Error, "CPU cook failed to produce an IBL payload");
                return;
            }

            TaskManager::RunInRenderThread([this, pass, alive, payload = std::move(result.payload)]() {
                // the render thread also destroys this object, so the check cannot race
                if (!alive->load())
                {
                    return;
                }

                if (pass->ApplyArtifact(payload))
                {
                    render_resource_change_event_.Trigger();
                }
                else
                {
                    Log(Error, "cooked IBL payload does not match the target resource layout");
                }
            });
        }));
    };

    request(ibl_brdf_pass_.get(), std::move(brdf_job));
    request(ibl_diffuse_pass_.get(), std::move(diffuse_job));
    request(ibl_specular_pass_.get(), std::move(specular_job));
}

void ImageBasedLighting::CookOnTheFly(const RenderConfig &config)
{
    if (cpu_cook_pending_)
    {
        return;
    }

    const unsigned step_budget = GetAdaptiveCookStepBudget(config);
    const std::array<IBLPass *, 3> ibl_passes{ibl_brdf_pass_.get(), ibl_diffuse_pass_.get(), ibl_specular_pass_.get()};

    std::array<size_t, 3> active_pass_indices{};
    size_t active_pass_count = 0;
    for (size_t i = 0; i < ibl_passes.size(); ++i)
    {
        const size_t pass_index = (next_cook_pass_index_ + i) % ibl_passes.size();
        auto *pass = ibl_passes[pass_index];

        if (!pass || pass->IsReady())
        {
            continue;
        }

        active_pass_indices[active_pass_count++] = pass_index;
    }

    if (active_pass_count == 0)
    {
        return;
    }

    const unsigned base_samples = step_budget / static_cast<unsigned>(active_pass_count);
    const unsigned extra_samples = step_budget % static_cast<unsigned>(active_pass_count);

    for (size_t i = 0; i < active_pass_count; ++i)
    {
        const unsigned samples_per_dispatch = base_samples + (i < extra_samples ? 1u : 0u);
        if (samples_per_dispatch == 0)
        {
            continue;
        }

        auto *pass = ibl_passes[active_pass_indices[i]];
        pass->CookOnTheFly(config, samples_per_dispatch);
    }

    next_cook_pass_index_ = static_cast<uint8_t>((next_cook_pass_index_ + 1u) % ibl_passes.size());
}

unsigned ImageBasedLighting::GetAdaptiveCookStepBudget(const RenderConfig &config)
{
    constexpr unsigned MinCookStepsPerFrame = 1;
    constexpr unsigned MaxCookStepsPerFrame = 64;
    constexpr float IncreaseThresholdRatio = 0.7f;

    if (!rhi_)
    {
        return cook_steps_per_frame_;
    }

    const auto frame_index = rhi_->GetFrameIndex();
    const float last_frame_gpu_time_ms = rhi_->GetFrameStats(frame_index).elapsed_time_ms;
    if (last_frame_gpu_time_ms <= 0.f)
    {
        return cook_steps_per_frame_;
    }

    const float target_frame_rate = std::max(30.f, config.target_framerate);
    const float target_frame_time_ms = 1000.f / target_frame_rate;

    if (last_frame_gpu_time_ms > target_frame_time_ms)
    {
        // Reduce load fast when frame time exceeds budget.
        cook_steps_per_frame_ = std::max(MinCookStepsPerFrame, cook_steps_per_frame_ / 2u);
    }
    else if (last_frame_gpu_time_ms < target_frame_time_ms * IncreaseThresholdRatio)
    {
        // Increase slowly to avoid oscillation.
        cook_steps_per_frame_ = std::min(MaxCookStepsPerFrame, cook_steps_per_frame_ + 1u);
    }

    return cook_steps_per_frame_;
}
} // namespace sparkle
