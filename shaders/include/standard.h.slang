#ifndef STANDARD_H_
#define STANDARD_H_

struct MaterialParameters
{
    float3 base_color;
    int base_color_texture;
    float3 emissive_color;
    int emissive_texture;
    float metallic;
    float roughness;
    int metallic_roughness_texture;
    int normal_texture;
    float eta;
};

struct RenderConfig
{
    uint mode;
    uint use_ssao;
    uint has_diffuse_ibl;
    uint has_specular_ibl;
};

#endif // STANDARD_H_