#pragma once

#include "core/Exception.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace sparkle
{
class SceneNode;
class PrimitiveComponent;
class CameraComponent;
class SceneRenderProxy;
class Material;
class DirectionalLight;
class SkyLight;

class Scene
{
public:
    explicit Scene();

    ~Scene();

    [[nodiscard]] SceneNode *GetRootNode() const
    {
        return root_node_.get();
    }

    void Cleanup();

    void Tick();

    void ProcessChange();

    void RegisterMaterial(Material *material);

    void UnregisterMaterial(const std::shared_ptr<Material> &material);

    void RegisterPrimitive(PrimitiveComponent *primitive);

    void UnregisterPrimitive(PrimitiveComponent *primitive);

    [[nodiscard]] bool BoxCollides(const PrimitiveComponent *primitive) const;

    void RecreateRenderProxy();

    SceneRenderProxy *GetRenderProxy()
    {
        ASSERT(render_proxy_);
        return render_proxy_.get();
    }

#pragma region Unique Components

    void SetMainCamera(const std::shared_ptr<CameraComponent> &main_camera);

    [[nodiscard]] CameraComponent *GetMainCamera() const
    {
        return main_camera_;
    }

    void SetDirectionalLight(DirectionalLight *light)
    {
        directional_light_ = light;
    }

    [[nodiscard]] DirectionalLight *GetDirectionalLight() const
    {
        return directional_light_;
    }

    void SetSkyLight(SkyLight *light)
    {
        sky_light_ = light;
    }

    [[nodiscard]] SkyLight *GetSkyLight() const
    {
        return sky_light_;
    }

#pragma endregion

private:
    static std::unique_ptr<SceneRenderProxy> CreateRenderProxy();

    std::unique_ptr<SceneRenderProxy> render_proxy_;
    std::unique_ptr<SceneNode> root_node_;

    std::unordered_set<PrimitiveComponent *> primitives_;
    std::unordered_map<Material *, uint32_t> material_usage_;

    // every scene must have a main camera
    CameraComponent *main_camera_ = nullptr;

    // if multiple directional lights exist, only the first one takes effect
    DirectionalLight *directional_light_ = nullptr;

    // if multiple sky lights exist, only the first one takes effect
    SkyLight *sky_light_ = nullptr;
};
} // namespace sparkle
