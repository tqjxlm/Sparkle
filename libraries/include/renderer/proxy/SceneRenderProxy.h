#pragma once

#include "core/RenderProxy.h"

#include <unordered_set>
#include <vector>

namespace sparkle
{
class CameraRenderProxy;
class PrimitiveRenderProxy;
class MaterialRenderProxy;
class Scene;
class TLAS;
class Ray;
class Intersection;
class SkyRenderProxy;
class DirectionalLightRenderProxy;
class BindlessManager;

class SceneRenderProxy : public RenderProxy
{
public:
    enum class PrimitiveChangeType : uint8_t
    {
        New,
        Remove,
        Move,
        Update,
    };

    struct PrimitiveChange
    {
        uint32_t from_id = std::numeric_limits<uint32_t>::max();
        uint32_t to_id = std::numeric_limits<uint32_t>::max();
        PrimitiveChangeType type;
    };

    SceneRenderProxy();

    ~SceneRenderProxy() override;

#pragma region SceneComponents

    [[nodiscard]] const auto &GetPrimitives() const
    {
        return primitives_;
    }

    [[nodiscard]] const auto &GetPrimitiveChangeList() const
    {
        return primitive_changes_;
    }

    [[nodiscard]] const auto &GetMaterialProxies() const
    {
        return materials_;
    }

    [[nodiscard]] const auto &GetNewMaterialProxiesThisFrame() const
    {
        return new_materials_;
    }

    RenderProxy *AddRenderProxy(std::unique_ptr<RenderProxy> &&proxy);

    void RemoveRenderProxy(RenderProxy *proxy);

    MaterialRenderProxy *AddMaterial(std::unique_ptr<MaterialRenderProxy> &&material);

    void RemoveMaterial(MaterialRenderProxy *material);

    void SetCamera(CameraRenderProxy *camera)
    {
        camera_ = camera;
    }

    [[nodiscard]] CameraRenderProxy *GetCamera() const
    {
        return camera_;
    }

    void SetSkyLight(SkyRenderProxy *sky_proxy)
    {
        sky_proxy_ = sky_proxy;
    }

    [[nodiscard]] SkyRenderProxy *GetSkyLight() const
    {
        return sky_proxy_;
    }

    [[nodiscard]] BindlessManager *GetBindlessManager() const
    {
        return bindless_manager_.get();
    }

    void SetDirectionalLight(DirectionalLightRenderProxy *directional_light)
    {
        directional_light_ = directional_light;
    }

    [[nodiscard]] DirectionalLightRenderProxy *GetDirectionalLight() const
    {
        return directional_light_;
    }

#pragma endregion

#pragma region BVH

    template <bool AnyHit> void Intersect(const Ray &ray, Intersection &intersection) const;

    void UpdateBVH();

#pragma endregion

#pragma region RenderProxy interface

    void Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config) override;

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config) override;

#pragma endregion

    void EndUpdate(RHIContext *rhi);

private:
    void RegisterPrimitive(PrimitiveRenderProxy *primitive);

    void UnregisterPrimitive(PrimitiveRenderProxy *primitive);

    CameraRenderProxy *camera_ = nullptr;
    SkyRenderProxy *sky_proxy_ = nullptr;
    DirectionalLightRenderProxy *directional_light_ = nullptr;

    std::unique_ptr<BindlessManager> bindless_manager_;

    std::vector<std::unique_ptr<RenderProxy>> proxies_;
    std::vector<std::unique_ptr<RenderProxy>> deleted_proxies_;

    std::vector<PrimitiveRenderProxy *> primitives_;
    std::vector<PrimitiveChange> primitive_changes_;

    std::vector<std::unique_ptr<MaterialRenderProxy>> materials_;
    std::unordered_set<MaterialRenderProxy *> new_materials_;
    std::vector<std::unique_ptr<MaterialRenderProxy>> deleted_materials_;
    std::unordered_set<uint32_t> free_material_ids_;

    std::unique_ptr<TLAS> tlas_;

    bool need_bvh_ = false;
    bool need_bvh_update_ = true;
};

extern template void SceneRenderProxy::Intersect<true>(const Ray &ray, Intersection &intersection) const;
extern template void SceneRenderProxy::Intersect<false>(const Ray &ray, Intersection &intersection) const;
} // namespace sparkle
