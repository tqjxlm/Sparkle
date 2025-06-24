#include "renderer/resource/ImageBasedLighting.h"

#include "core/Profiler.h"
#include "renderer/RenderConfig.h"
#include "renderer/pass/IBLBrdfPass.h"
#include "renderer/pass/IBLDiffusePass.h"
#include "renderer/pass/IBLSpecularPass.h"

namespace sparkle
{
ImageBasedLighting::ImageBasedLighting(const RHIResourceRef<RHIImage> &env_map) : env_map_(env_map)
{
}

ImageBasedLighting::~ImageBasedLighting() = default;

void ImageBasedLighting::InitRenderResources(RHIContext *ctx, const RenderConfig &config)
{
    ASSERT(env_map_);

    PROFILE_SCOPE_LOG("ImageBasedLighting::InitRenderResources");

    ibl_brdf_pass_ = PipelinePass::Create<IBLBrdfPass>(config, ctx);
    ibl_diffuse_pass_ = PipelinePass::Create<IBLDiffusePass>(config, ctx, env_map_);
    ibl_specular_pass_ = PipelinePass::Create<IBLSpecularPass>(config, ctx, env_map_);
}

void ImageBasedLighting::CookOnTheFly(const RenderConfig &config)
{
    if (ibl_brdf_pass_ && !ibl_brdf_pass_->IsReady())
    {
        ibl_brdf_pass_->CookOnTheFly(config);
        if (ibl_brdf_pass_->IsReady())
        {
            render_resource_change_event_.Trigger();
        }
    }

    if (ibl_diffuse_pass_ && !ibl_diffuse_pass_->IsReady())
    {
        ibl_diffuse_pass_->CookOnTheFly(config);
        if (ibl_diffuse_pass_->IsReady())
        {
            render_resource_change_event_.Trigger();
        }
    }

    if (ibl_specular_pass_ && !ibl_specular_pass_->IsReady())
    {
        ibl_specular_pass_->CookOnTheFly(config);
        if (ibl_specular_pass_->IsReady())
        {
            render_resource_change_event_.Trigger();
        }
    }
}
}; // namespace sparkle
