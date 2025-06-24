#include "debug.h.hlsl"
#include "depth.h.hlsl"
#include "directional_light.h.hlsl"
#include "gbuffer.h.hlsl"
#include "pbr.h.hlsl"
#include "screen.h.hlsl"
#include "shadow.h.hlsl"
#include "sky_light.h.hlsl"
#include "ssao.h.hlsl"
#include "standard.h.hlsl"
#include "view.h.hlsl"

[[vk::binding(1, 0)]] cbuffer view : register(b0)
{
    View view;
}

[[vk::binding(0, 1)]] cbuffer ubo : register(b2)
{
    SkyLight sky_light;
    DirectionalLight dir_light;
    float3 camera_pos;
    RenderConfig config;
    SSAOConfig ssao_config;
}

[[vk::binding(1, 1)]] Texture2D shadow_map : register(t0);
[[vk::binding(2, 1)]] SamplerState shadow_map_sampler : register(s0);

[[vk::binding(3, 1)]] Texture2D ibl_brdf : register(t2);
[[vk::binding(4, 1)]] SamplerState ibl_brdf_sampler : register(s2);
[[vk::binding(5, 1)]] TextureCube ibl_diffuse : register(t3);
[[vk::binding(6, 1)]] SamplerState ibl_diffuse_sampler : register(s3);
[[vk::binding(7, 1)]] TextureCube ibl_specular : register(t4);
[[vk::binding(8, 1)]] SamplerState ibl_specular_sampler : register(s4);

[[vk::binding(0, 2)]] Texture2D<uint4> gbuffer_texture : register(t5);
[[vk::binding(1, 2)]] Texture2D<float> depth_texture : register(t6);
[[vk::binding(2, 2)]] SamplerState depth_sampler : register(s5);

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
