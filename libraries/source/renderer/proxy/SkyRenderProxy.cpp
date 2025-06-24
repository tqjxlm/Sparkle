#include "renderer/proxy/SkyRenderProxy.h"

#include "core/math/Ray.h"
#include "io/Image.h"
#include "rhi/RHI.h"

namespace sparkle
{

SkyRenderProxy::SkyRenderProxy(const Image2DCube *sky_map) : sky_map_raw_(sky_map)
{
}

void SkyRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    LightRenderProxy::InitRenderResources(rhi, config);

    if (sky_map_raw_ != nullptr)
    {
        sky_map_ = rhi->CreateTextureCube(sky_map_raw_, "SkyMap_" + sky_map_raw_->GetName());
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
