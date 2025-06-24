#pragma once

#include "io/Material.h"
#include "rhi/RHIImage.h"

namespace sparkle
{
class Material;
class Ray;
class SceneRenderProxy;
struct RenderConfig;

class MaterialRenderProxy
{
public:
    explicit MaterialRenderProxy(const MaterialResource &raw_material);

    virtual ~MaterialRenderProxy();

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config);

    void SetScene(SceneRenderProxy *scene)
    {
        scene_proxy_ = scene;
    }

    [[nodiscard]] const auto &GetName() const
    {
        return raw_material_.name;
    }

#pragma region Graphics Render

    struct alignas(16) MaterialRenderData
    {
        Vector3 baseColor;
        uint32_t baseColorTextureId = UINT_MAX;
        Vector3 emissiveColor;
        uint32_t emissiveTextureId = UINT_MAX;
        float metallic;
        float roughness;
        uint32_t metallicRoughnessTextureId = UINT_MAX;
        uint32_t normalTextureId = UINT_MAX;
        float eta;

        explicit MaterialRenderData(const MaterialResource &material);
    };

    [[nodiscard]] MaterialRenderData GetRenderData() const
    {
        return render_data_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetBaseColorTexture() const
    {
        return base_color_texture_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetNormalTexture() const
    {
        return normal_texture_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetMetallicRoughnessTexture() const
    {
        return metallic_roughness_texture_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetEmissiveTexture() const
    {
        return emissive_texture_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetUBO() const
    {
        return parameter_buffer_;
    }

    void PrintSample(const Vector2 &tex_coord) const;

#pragma endregion

#pragma region Material Instance

    void SetIndex(uint32_t index)
    {
        render_index_ = index;
    }

    void Destroy();

    [[nodiscard]] uint32_t GetRenderIndex() const
    {
        ASSERT_F(render_index_ != UINT_MAX, "Material not registered. {}", GetName());
        return render_index_;
    }

#pragma endregion

#pragma region CPU Render

    // all directions should be in world space
    virtual Vector3 SampleSurface(const Ray &ray, Vector3 &w_i, const Vector3 &normal, const Vector3 &tangent,
                                  const Vector2 &uv) const = 0;

    [[nodiscard]] Vector3 GetBaseColor(const Vector2 &uv) const
    {
        if (raw_material_.base_color_texture)
        {
            return raw_material_.base_color_texture->Sample(uv).cwiseProduct(raw_material_.base_color);
        }
        return raw_material_.base_color;
    }

    [[nodiscard]] float GetMetallic(const Vector2 &uv) const
    {
        if (raw_material_.metallic_roughness_texture)
        {
            return raw_material_.metallic_roughness_texture->Sample(uv).z() * raw_material_.metallic;
        }
        return raw_material_.metallic;
    }

    [[nodiscard]] float GetRoughness(const Vector2 &uv) const
    {
        if (raw_material_.metallic_roughness_texture)
        {
            return raw_material_.metallic_roughness_texture->Sample(uv).y() * raw_material_.roughness;
        }
        return raw_material_.roughness;
    }

    [[nodiscard]] Vector3 GetNormal(const Vector2 &uv) const
    {
        if (!HasNormalTexture())
        {
            return Zeros;
        }

        return raw_material_.normal_texture->Sample(uv) * 2 - Ones;
    }

    [[nodiscard]] Vector3 GetEmissive(const Vector2 &uv) const
    {
        if (raw_material_.emissive_texture)
        {
            return raw_material_.emissive_texture->Sample(uv).cwiseProduct(raw_material_.emissive_color);
        }

        return raw_material_.emissive_color;
    }

    [[nodiscard]] bool HasNormalTexture() const
    {
        return raw_material_.normal_texture != nullptr;
    }

    [[nodiscard]] float GetEta() const
    {
        return raw_material_.eta;
    }

#pragma endregion

private:
    RHIResourceRef<RHIImage> CreateAndRegisterTexture(RHIContext *rhi, const std::shared_ptr<const Image2D> &image,
                                                      uint32_t &out_id, const std::string &name);

    const MaterialResource &raw_material_;

    RHIResourceRef<RHIImage> base_color_texture_;
    RHIResourceRef<RHIImage> normal_texture_;
    RHIResourceRef<RHIImage> metallic_roughness_texture_;
    RHIResourceRef<RHIImage> emissive_texture_;

    RHIResourceRef<RHIBuffer> parameter_buffer_;

    MaterialRenderData render_data_;

    uint32_t render_index_ = UINT_MAX;

    uint32_t use_bindless_ : 1 = 0;
    uint32_t rhi_initialized_ : 1 = 0;

    SceneRenderProxy *scene_proxy_ = nullptr;
};
} // namespace sparkle
