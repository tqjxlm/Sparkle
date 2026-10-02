#include "color_slot.h.slang"
#include "debug.h.slang"
#include "depth.h.slang"
#include "directional_light.h.slang"
#include "gbuffer.h.slang"
#include "pbr.h.slang"
#include "screen.h.slang"
#include "shadow.h.slang"
#include "sky_light.h.slang"
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
}

[[vk::binding(1, 1)]] Texture2D shadow_map;
[[vk::binding(2, 1)]] SamplerState shadow_map_sampler;

[[vk::binding(3, 1)]] Texture2D ibl_brdf;
[[vk::binding(4, 1)]] SamplerState ibl_brdf_sampler;
[[vk::binding(5, 1)]] TextureCube ibl_diffuse;
[[vk::binding(6, 1)]] SamplerState ibl_diffuse_sampler;
[[vk::binding(7, 1)]] TextureCube ibl_specular;
[[vk::binding(8, 1)]] SamplerState ibl_specular_sampler;

// PIXEL_LOCAL reads the GBuffer and the depth copy the GBuffer pass wrote at this pixel (ColorSlot::GBufferPacked,
// ColorSlot::DepthCopy)
#ifdef PIXEL_LOCAL
[[vk::binding(0, 2)]] [[vk::input_attachment_index(2)]] SubpassInput<uint4> gbuffer_texture;
[[vk::binding(1, 2)]] [[vk::input_attachment_index(3)]] SubpassInput depth_texture;
#else
[[vk::binding(0, 2)]] Texture2D<uint4> gbuffer_texture;
[[vk::binding(1, 2)]] Texture2D<float> depth_texture;
#endif

float4 Shade(VSInterpolant vs_in, uint4 gbuffer_sample, float depth)
{
    SurfaceAttributes surface = DecodeSurfaceFromGBuffer(gbuffer_sample);

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
        return debug_color;
    case 2:
        return float4(0, 0, 0, 1.0);
    case 3:
        return float4(VisualizeVector(surface.normal), 1.0);
    case 4: {
        float3 specular_direction = reflect(-V, surface.normal);
        return float4(VisualizeVector(specular_direction), 1.0);
    }
    case 5:
        return float4(ambient, 1.0);
    case 6:
        return float4(directional, 1.0);
    case 7:
        return float4(surface.metallic, surface.metallic, surface.metallic, 1.0);
    case 8:
        return float4(surface.roughness, surface.roughness, surface.roughness, 1.0);
    case 9:
        return float4(surface.albedo, 1.0);
    case 10:
        return float4(surface.emissive, 1.0);
    case 11:
        return float4(VisualizeDepth(depth, view), 1.0);
    default:
        break;
    }

    return float4(ambient + directional + surface.emissive, 1.0f);
}

[shader("fragment")] SceneColorOutput shader_main(VSInterpolant vs_in) {
#ifdef PIXEL_LOCAL
    uint4 gbuffer_sample = gbuffer_texture.SubpassLoad();
    float depth = depth_texture.SubpassLoad().r;
#else
    int3 pixel = int3(int2(vs_in.position.xy), 0);
    uint4 gbuffer_sample = gbuffer_texture.Load(pixel);
    float depth = depth_texture.Load(pixel);
#endif
    return {Shade(vs_in, gbuffer_sample, depth)};
}