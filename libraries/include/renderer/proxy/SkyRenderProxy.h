#pragma once

#include "renderer/proxy/LightRenderProxy.h"

#include "rhi/RHIImage.h"

namespace sparkle
{
class Image2DCube;

class SkyRenderProxy : public LightRenderProxy
{
public:
    static constexpr Scalar MaxBrightness = 100.0f;
    static constexpr Scalar MaxIBLBrightness = 10.f;

    struct UniformBufferData
    {
        alignas(16) Vector3 color = Zeros;
        uint32_t has_sky_map = 0;
    };

    explicit SkyRenderProxy(const Image2DCube *sky_map);

#pragma region RenderProxy interface

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config) override;

#pragma endregion

    [[nodiscard]] RHIResourceRef<RHIImage> GetSkyMap() const
    {
        return sky_map_;
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

    const Image2DCube *sky_map_raw_;
};
} // namespace sparkle
