#pragma once

#include "core/task/TaskFuture.h"
#include "scene/component/light/LightSource.h"

namespace sparkle
{
class CookHandle;
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

    // only takes effect when no sky map is set
    [[nodiscard]] const Vector3 &GetColor() const
    {
        return color_;
    }

    void SetSkyMap(const std::string &file_path);

    [[nodiscard]] bool HasSkyMap() const
    {
        return !sky_map_path_.empty();
    }

    // the file path passed to SetSkyMap. empty if no sky map is set.
    [[nodiscard]] const std::string &GetSkyMapPath() const
    {
        return sky_map_path_;
    }

    // manifest key of the cooked sky map cube, produced as a scene-load side effect;
    // cook plans must record it so packaged targets ship the artifact
    [[nodiscard]] std::string GetCookManifestKey() const;

    void OnAttach() override;

    [[nodiscard]] const std::shared_ptr<Image2DCube> &GetCubeMap() const
    {
        return cube_map_;
    }

    // Ready on the main thread once the sky map artifact was applied or resolution failed.
    // Only valid after a sky map light is attached.
    [[nodiscard]] const std::shared_ptr<TaskFuture<>> &OnCooked() const;

    [[nodiscard]] Vector3 GetSunBrightness() const
    {
        ASSERT(cube_map_);
        return sun_brightness_;
    }

    [[nodiscard]] Vector3 GetSunDirection() const
    {
        ASSERT(cube_map_);
        return sun_direction_;
    }

protected:
    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

private:
    void RequestCook();

    void ApplyCookedData(const std::vector<char> &payload);

    Vector3 color_ = Vector3(0.5f, 0.7f, 1.0f);

    std::string sky_map_path_;

    std::shared_ptr<Image2DCube> cube_map_;

    std::unique_ptr<CookHandle> cook_handle_;

    Vector3 sun_brightness_ = Ones;

    Vector3 sun_direction_ = Vector3(0, 1, 0);
};
} // namespace sparkle
