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

    // what passes sample the sky map, and the environment maps the IBL cooks integrate, with
    static constexpr RHISampler::SamplerAttribute SkyMapSampler{
        .address_mode = RHISampler::SamplerAddressMode::Repeat,
        .filtering_method_min = RHISampler::FilteringMethod::Linear,
        .filtering_method_mag = RHISampler::FilteringMethod::Linear,
        .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};

    explicit SkyRenderProxy(std::shared_ptr<const Image2DCube> sky_map);

    ~SkyRenderProxy() override;

#pragma region RenderProxy interface

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

    // the gpu path tracer's alias table over the sky map's luminance, with GetSamplingResolution()^2 texels per cube
    // face in face order. null, at resolution 0, without a lit sky map or outside the gpu pipeline
    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetSamplingTable() const
    {
        return sampling_table_;
    }

    [[nodiscard]] uint32_t GetSamplingResolution() const
    {
        return sampling_resolution_;
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
    void CreateSamplingTable(RHIContext *rhi);

    UniformBufferData ubo_;

    RHIResourceRef<RHIImage> sky_map_;

    std::shared_ptr<const Image2DCube> sky_map_raw_;

    std::unique_ptr<ImageBasedLighting> image_based_lighting_;

    RHIResourceRef<RHIBuffer> sampling_table_;
    uint32_t sampling_resolution_ = 0;
};
} // namespace sparkle
