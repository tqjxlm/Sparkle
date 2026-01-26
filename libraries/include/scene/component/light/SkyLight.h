#pragma once

#include "scene/component/light/LightSource.h"

namespace sparkle
{
class Image2D;
class Image2DCube;

class SkyLight : public LightSourceComponent
{
public:
    SkyLight();

    ~SkyLight() override;

    void SetColor(const Vector3 &color)
    {
        color_ = color;
    }

    void SetSkyMap(const std::string &file_path);

    [[nodiscard]] const Image2D *GetSkyMap() const
    {
        return sky_map_.get();
    }

    void OnAttach() override;

    [[nodiscard]] bool IsCooked() const
    {
        return cooked_;
    }

    void Cook();

    [[nodiscard]] Vector3 GetSunBrightness() const
    {
        ASSERT(cooked_);
        return sun_brightness_;
    }

    [[nodiscard]] Vector3 GetSunDirection() const
    {
        ASSERT(cooked_);
        return sun_direction_;
    }

protected:
    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

private:
    void LogCookStatus() const;

    Vector3 color_ = Vector3(0.5f, 0.7f, 1.0f);

    std::unique_ptr<Image2D> sky_map_;

    std::unique_ptr<Image2D> specular_map_;

    std::unique_ptr<Image2DCube> cube_map_;

    bool cooked_ = false;
    std::atomic<int32_t> cooked_row_count_ = 0;

    Vector3 sun_brightness_ = Ones;

    Vector3 sun_direction_ = Vector3(0, 1, 0);
};
} // namespace sparkle
