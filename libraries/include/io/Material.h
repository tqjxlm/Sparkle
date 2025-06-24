#pragma once

#include <io/Image.h>

namespace sparkle
{
struct MaterialResource
{
    Vector3 base_color = Ones;
    Vector3 emissive_color = Zeros;

    float metallic = 1.0f;
    float roughness = 1.0f;
    float eta = 0.0f;

    std::shared_ptr<Image2D> base_color_texture = nullptr;
    std::shared_ptr<Image2D> emissive_texture = nullptr;
    std::shared_ptr<Image2D> metallic_roughness_texture = nullptr;
    std::shared_ptr<Image2D> normal_texture = nullptr;

    std::string name;
};
} // namespace sparkle
