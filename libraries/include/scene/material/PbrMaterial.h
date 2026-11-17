#pragma once

#include "scene/material/Material.h"

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
    std::unique_ptr<MaterialRenderProxy> CreateRenderProxyInternal() override;
};
} // namespace sparkle
