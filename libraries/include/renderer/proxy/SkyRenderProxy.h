#pragma once

#include "renderer/proxy/LightRenderProxy.h"

#include "rhi/RHIImage.h"

namespace sparkle
{
class Image2DCube;
class ImageBasedLighting;

class SkyRenderProxy : public LightRenderProxy
{
public:
    // must match SkyLight in shaders/include/sky_light.h.slang
    struct UniformBufferData
    {
        uint32_t has_sky_map = 0;
        alignas(16) Vector3 color = Zeros;
    };

    explicit SkyRenderProxy(std::shared_ptr<const Image2DCube> sky_map);

    ~SkyRenderProxy() override;

#pragma region RenderProxy interface

    void Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config) override;

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config) override;

#pragma endregion

    [[nodiscard]] RHIResourceRef<RHIImage> GetSkyMap() const
    {
        return sky_map_;
    }

    [[nodiscard]] ImageBasedLighting *GetImageBasedLighting() const
    {
        return image_based_lighting_.get();
    }

    void SetData(const Vector3 &color)
    {
        ubo_.color = color;
        ubo_.has_sky_map = static_cast<uint32_t>(sky_map_raw_ != nullptr);
    }

    [[nodiscard]] UniformBufferData GetRenderData() const
    {
        return ubo_;
    }

#pragma region CPU Render

    [[nodiscard]] Vector3 Evaluate(const Ray &ray) const override;

    void Sample(const Vector3 & /*origin*/, Vector3 & /*direction*/) const override
    {
        UnImplemented();
    }

#pragma endregion

private:
    UniformBufferData ubo_;

    RHIResourceRef<RHIImage> sky_map_;

    std::shared_ptr<const Image2DCube> sky_map_raw_;

    std::unique_ptr<ImageBasedLighting> image_based_lighting_;
};
} // namespace sparkle
