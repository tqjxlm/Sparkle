#include "debug.h.slang"
#include "depth.h.slang"
#include "directional_light.h.slang"
#include "mesh.h.slang"
#include "pbr.h.slang"
#include "shadow.h.slang"
#include "sky_light.h.slang"
#include "ssao.h.slang"
#include "standard.h.slang"
#include "view.h.slang"

[[vk::binding(0, 0)]] cbuffer view
{
    View view;
}

[[vk::binding(0, 1)]] cbuffer mesh
{
    matrix ModelMatrix;
    matrix ModelMatrixInvTranspose;
}

[[vk::binding(0, 2)]] cbuffer ubo
{
    SkyLight sky_light;
    DirectionalLight dir_light;
    float3 camera_pos;
    RenderConfig config;
    SSAOConfig ssao_config;
}

[[vk::binding(1, 2)]] Texture2D shadow_map;
[[vk::binding(2, 2)]] SamplerState shadow_map_sampler;

[[vk::binding(3, 2)]] Texture2D prepass_depth_map;
[[vk::binding(4, 2)]] SamplerState prepass_depth_map_sampler;

[[vk::binding(5, 2)]] Texture2D ibl_brdf;
[[vk::binding(6, 2)]] SamplerState ibl_brdf_sampler;
[[vk::binding(7, 2)]] TextureCube ibl_diffuse;
[[vk::binding(8, 2)]] SamplerState ibl_diffuse_sampler;
[[vk::binding(9, 2)]] TextureCube ibl_specular;
[[vk::binding(10, 2)]] SamplerState ibl_specular_sampler;

// set 3 for material info that changes per mesh but may be shared if ordered
[[vk::binding(0, 3)]] cbuffer material
{
    MaterialParameters mat_param;
}

[[vk::binding(1, 3)]] Texture2D base_color_texture;
[[vk::binding(2, 3)]] Texture2D normal_texture;
[[vk::binding(3, 3)]] Texture2D metallic_roughness_texture;
[[vk::binding(4, 3)]] Texture2D emissive_texture;
[[vk::binding(5, 3)]] SamplerState material_texture_sampler;

float3 SampleNormal(float2 tex_coord)
{
    return normal_texture.Sample(material_texture_sampler, tex_coord).xyz * 2.0 - 1.0;
}

SurfaceAttributes LoadSurface(MeshPassVSInterpolant vs_in)
{
    SurfaceAttributes surface;

    if (mat_param.normal_texture >= 0)
    {
        float3 mapped = TransformBasisToWorld(SampleNormal(vs_in.uv), vs_in.tangent, vs_in.bi_tangent, vs_in.normal);
        float len_sq = dot(mapped, mapped);
        surface.normal = len_sq > Eps ? mapped * rsqrt(len_sq) : vs_in.normal;
    }
    else
    {
        surface.normal = vs_in.normal;
    }

    float3 transformed_normal = mul((float3x3)ModelMatrixInvTranspose, surface.normal);
    float tn_len_sq = dot(transformed_normal, transformed_normal);
    surface.normal = tn_len_sq > Eps ? transformed_normal * rsqrt(tn_len_sq) : float3(0, 0, 1);

    float4 metallic_roughness_sampled = mat_param.metallic_roughness_texture >= 0
                                            ? metallic_roughness_texture.Sample(material_texture_sampler, vs_in.uv)
                                            : (float4)1;

    surface.roughness = metallic_roughness_sampled.y * mat_param.roughness;
    surface.metallic = metallic_roughness_sampled.z * mat_param.metallic;

    float3 base_color_sampled = mat_param.base_color_texture >= 0
                                    ? base_color_texture.Sample(material_texture_sampler, vs_in.uv).rgb
                                    : (float3)1;
    surface.albedo = mat_param.base_color * base_color_sampled;

    float3 emissive_sampled =
        mat_param.emissive_texture >= 0 ? emissive_texture.Sample(material_texture_sampler, vs_in.uv).rgb : (float3)1;
    surface.emissive = mat_param.emissive_color * emissive_sampled;

    return surface;
}

[shader("fragment")]
void main(MeshPassVSInterpolant vs_in, out float4 color_out : SV_Target)
{
    SurfaceAttributes surface = LoadSurface(vs_in);

    float3 V = normalize(camera_pos - vs_in.world_pos);

    IBLConfig ibl;
    ibl.diffuse = ibl_diffuse;
    ibl.diffuse_sampler = ibl_diffuse_sampler;
    ibl.specular = ibl_specular;
    ibl.specular_sampler = ibl_specular_sampler;
    ibl.brdf = ibl_brdf;
    ibl.brdf_sampler = ibl_brdf_sampler;
    float3 ambient = CalculateAmbientLighting(config, V, sky_light, surface, ibl);
    if (config.use_ssao != 0)
    {
        ambient *= CalculateSSAO(vs_in.world_pos, view.ViewProjectionMatrix, (float3x3)ModelMatrixInvTranspose,
                                 ssao_config, prepass_depth_map, prepass_depth_map_sampler, view.near, view.far);
    }

    bool in_directional_shadow =
        CalculateDirectionalLightShadow(surface, dir_light, vs_in.world_pos, shadow_map, shadow_map_sampler);

    float3 directional =
        in_directional_shadow ? (float3)0 : CalculateDirectLighting(V, dir_light.direction, dir_light.color, surface);

    // Debug output
    switch (config.mode)
    {
    case 1:
        color_out = debug_color;
        return;
    case 2:
        color_out = float4(0, 0, 0, 1.0);
        return;
    case 3:
        color_out = float4(VisualizeVector(surface.normal), 1.0);
        return;
    case 4: {
        float3 specular_direction = reflect(-V, surface.normal);
        color_out = float4(VisualizeVector(specular_direction), 1.0);
        return;
    }
    case 5:
        color_out = float4(ambient, 1.0);
        return;
    case 6:
        color_out = float4(directional, 1.0);
        return;
    case 7:
        color_out = float4(surface.metallic, surface.metallic, surface.metallic, 1.0);
        return;
    case 8:
        color_out = float4(surface.roughness, surface.roughness, surface.roughness, 1.0);
        return;
    case 9:
        color_out = float4(surface.albedo, 1.0);
        return;
    case 10:
        color_out = float4(surface.emissive, 1.0);
        return;
    case 11:
        color_out = float4(VisualizeDepth(vs_in.gl_Position.z, view), 1.0);
        return;
    default:
        break;
    }

    float3 result = ambient + directional + surface.emissive;
    if (any(isnan(result) || isinf(result)))
        result = (float3)0;
    color_out = float4(result, 1.0f);
}