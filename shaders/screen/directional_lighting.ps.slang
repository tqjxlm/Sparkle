#include "debug.h.slang"
#include "depth.h.slang"
#include "directional_light.h.slang"
#include "gbuffer.h.slang"
#include "pbr.h.slang"
#include "screen.h.slang"
#include "shadow.h.slang"
#include "sky_light.h.slang"
#include "ssao.h.slang"
#include "standard.h.slang"
#include "view.h.slang"

[[vk::binding(1, 0)]] cbuffer view
{
    View view;
}

[[vk::binding(0, 1)]] cbuffer ubo
{
    SkyLight sky_light;
    DirectionalLight dir_light;
    float3 camera_pos;
    RenderConfig config;
    SSAOConfig ssao_config;
}

[[vk::binding(1, 1)]] Texture2D shadow_map;
[[vk::binding(2, 1)]] SamplerState shadow_map_sampler;

[[vk::binding(3, 1)]] Texture2D ibl_brdf;
[[vk::binding(4, 1)]] SamplerState ibl_brdf_sampler;
[[vk::binding(5, 1)]] TextureCube ibl_diffuse;
[[vk::binding(6, 1)]] SamplerState ibl_diffuse_sampler;
[[vk::binding(7, 1)]] TextureCube ibl_specular;
[[vk::binding(8, 1)]] SamplerState ibl_specular_sampler;

[[vk::binding(0, 2)]] Texture2D<uint4> gbuffer_texture;
[[vk::binding(1, 2)]] Texture2D<float> depth_texture;
[[vk::binding(2, 2)]] SamplerState depth_sampler;

[shader("fragment")]
void main(VSInterpolant vs_in, out float4 color_out : SV_Target)
{
    uint2 gbuffer_size;
    gbuffer_texture.GetDimensions(gbuffer_size.x, gbuffer_size.y);
    int2 gbuffer_pixel_coords = int2(vs_in.uv * gbuffer_size);
    uint4 gbuffer_sample = gbuffer_texture.Load(int3(gbuffer_pixel_coords, 0));

    SurfaceAttributes surface = DecodeSurfaceFromGBuffer(gbuffer_sample);

    float depth = depth_texture.Sample(depth_sampler, vs_in.uv).r;

    float3 world_pos = GetWorldPositionFromDepth(depth, vs_in.uv, view);

    float3 V = normalize(camera_pos - world_pos);

    IBLConfig ibl;
    ibl.diffuse = ibl_diffuse;
    ibl.diffuse_sampler = ibl_diffuse_sampler;
    ibl.specular = ibl_specular;
    ibl.specular_sampler = ibl_specular_sampler;
    ibl.brdf = ibl_brdf;
    ibl.brdf_sampler = ibl_brdf_sampler;
    float3 ambient = CalculateAmbientLighting(config, V, sky_light, surface, ibl);

    bool in_directional_shadow =
        CalculateDirectionalLightShadow(surface, dir_light, world_pos, shadow_map, shadow_map_sampler);

    float3 directional =
        in_directional_shadow ? (float3)0 : CalculateDirectLighting(V, dir_light.direction, dir_light.color, surface);

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
        color_out = float4(VisualizeDepth(depth, view), 1.0);
        return;
    default:
        break;
    }

    color_out = float4(ambient + directional + surface.emissive, 1.0f);
}