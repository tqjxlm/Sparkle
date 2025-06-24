#pragma once

#include "scene/material/PbrMaterial.h"

namespace sparkle
{
// perfect metal
class MetalMaterial : public PbrMaterial
{
public:
    explicit MetalMaterial(const MaterialResource &raw_material) : PbrMaterial(raw_material)
    {
        raw_material_.metallic = 1.f;
        raw_material_.roughness = 0.2f;
    }
};
} // namespace sparkle
