#pragma once

#include "scene/material/PbrMaterial.h"

namespace sparkle
{
// perfect lambertian
class LambertianMaterial : public PbrMaterial
{
public:
    explicit LambertianMaterial(const MaterialResource &raw_material) : PbrMaterial(raw_material)
    {
        raw_material_.metallic = 0.f;
        raw_material_.roughness = 1.f;
    }
};
} // namespace sparkle
