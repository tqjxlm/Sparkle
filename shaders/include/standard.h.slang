#ifndef STANDARD_H_
#define STANDARD_H_

// field order is layout-critical: a float3 must not be followed by a scalar (std140/MSL divergence)
struct MaterialParameters
{
    int base_color_texture;
    int emissive_texture;
    int metallic_roughness_texture;
    int normal_texture;
    float metallic;
    float roughness;
    float eta;
    float3 base_color;
    float3 emissive_color;
};

struct RenderConfig
{
    uint mode;
    uint use_ssao;
    uint has_diffuse_ibl;
    uint has_specular_ibl;
};

#endif // STANDARD_H_