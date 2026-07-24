#pragma once

#include "core/cook/CookArtifact.h"
#include "core/task/TaskFuture.h"
#include "scene/component/light/LightSource.h"

#include <functional>
#include <future>

namespace sparkle
{
class CookHandle;
class Image2DCube;
class SceneAsyncTask;
struct CookResult;

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

    // identity of the fp16 master sky cube artifact: shared pool content that family
    // transcodes derive from; it does not ship in target images
    [[nodiscard]] static CookArtifactKey MasterCookKey(const std::string &sky_map_path);

    // build-time master cook (store hit or cook now). empty on failure
    [[nodiscard]] static std::vector<char> CookMasterPayload(const std::string &sky_map_path);

    // the cube map a sky payload carries (fp16 master or family transcode). null on a bad payload
    [[nodiscard]] static std::shared_ptr<Image2DCube> MakeCubeFromPayload(const std::vector<char> &payload,
                                                                          const std::string &sky_map_path);

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
    // retires the request chain exactly once, on the main thread
    using SkyCookFinish = std::function<void(bool)>;

    void RequestCook();

    void ProbeArtifact(const CookArtifactKey &key, const SkyCookFinish &finish, std::function<void()> on_miss);

    void RequestMasterCook(const SkyCookFinish &finish);

    void DeliverCookedResult(CookResult result, const SkyCookFinish &finish);

    void ApplyCookedResult(CookResult result, const SkyCookFinish &finish);

    [[nodiscard]] bool ApplyCookedData(const std::vector<char> &payload);

    Vector3 color_ = Vector3(0.5f, 0.7f, 1.0f);

    std::string sky_map_path_;

    std::shared_ptr<Image2DCube> cube_map_;

    std::unique_ptr<CookHandle> cook_handle_;

    // OnCooked future for the whole lookup chain: a per-request CookHandle delivery would
    // fire on an intermediate probe, before the payload is applied
    std::shared_ptr<std::promise<void>> cooked_promise_;
    std::shared_ptr<TaskFuture<>> cooked_future_;

    Vector3 sun_brightness_ = Ones;

    Vector3 sun_direction_ = Vector3(0, 1, 0);
};
} // namespace sparkle
