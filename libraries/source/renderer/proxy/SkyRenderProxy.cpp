#include "renderer/proxy/SkyRenderProxy.h"

#include "core/math/Ray.h"
#include "io/Image.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

namespace sparkle
{

SkyRenderProxy::SkyRenderProxy(std::shared_ptr<const Image2DCube> sky_map) : sky_map_raw_(std::move(sky_map))
{
}

SkyRenderProxy::~SkyRenderProxy() = default;

void SkyRenderProxy::Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config)
{
    LightRenderProxy::Update(rhi, camera, config);

    if (image_based_lighting_ && image_based_lighting_->NeedUpdate())
    {
        image_based_lighting_->CookOnTheFly(config);
    }
}

void SkyRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    LightRenderProxy::InitRenderResources(rhi, config);

    sky_map_ = nullptr;
    image_based_lighting_.reset();

    if (sky_map_raw_)
    {
        sky_map_ = rhi->CreateTextureCube(sky_map_raw_.get(), sky_map_raw_->GetName());

        const bool raster_ibl =
            config.pipeline == RenderConfig::Pipeline::Forward || config.pipeline == RenderConfig::Pipeline::Deferred;
        if (raster_ibl && (config.use_diffuse_ibl || config.use_specular_ibl))
        {
            image_based_lighting_ = std::make_unique<ImageBasedLighting>(sky_map_, sky_map_raw_);
            image_based_lighting_->InitRenderResources(rhi, config);
        }
    }

    ubo_.has_sky_map = sky_map_ ? 1 : 0;
}

Vector3 SkyRenderProxy::Evaluate(const Ray &ray) const
{
    auto d = ray.Direction();
    [[likely]] if (sky_map_)
    {
        auto sample = sky_map_raw_->Sample(d);
        return sample;
    }

    // top: color. bottom: white.
    const float t = 0.5f * d.z() + 0.5f;
    auto sky_light = utilities::Lerp(Ones, ubo_.color, t);

    return sky_light;
}
} // namespace sparkle
