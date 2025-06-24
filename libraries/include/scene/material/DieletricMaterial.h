#pragma once

#include "scene/material/Material.h"

#include "renderer/proxy/DialetricMaterialRenderProxy.h"

namespace sparkle
{
// perfect dieletric (i.e. smooth glass)
class DieletricMaterial : public Material
{
public:
    explicit DieletricMaterial(const MaterialResource &material_resource) : Material(material_resource)
    {
        type_ = Type::Dieletric;
        raw_material_.roughness = 0.f;
        raw_material_.metallic = 0.f;
    }

protected:
    std::unique_ptr<MaterialRenderProxy> CreateRenderProxyInternal() override
    {
        return std::make_unique<DieletricMaterialRenderProxy>(raw_material_);
    }
};
} // namespace sparkle
