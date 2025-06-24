#pragma once

#include "scene/material/Material.h"

#include "renderer/proxy/PbrMaterialRenderProxy.h"

namespace sparkle
{
class PbrMaterial : public Material
{
public:
    explicit PbrMaterial(const MaterialResource &raw_material) : Material(raw_material)
    {
        type_ = Type::PBR;
    }

protected:
    std::unique_ptr<MaterialRenderProxy> CreateRenderProxyInternal() override
    {
        return std::make_unique<PbrMaterialRenderProxy>(raw_material_);
    }
};
} // namespace sparkle
