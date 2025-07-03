#ifndef SHADOW_H_
#define SHADOW_H_

#include "directional_light.h.slang"
#include "surface.h.slang"

bool CalculateDirectionalLightShadow(SurfaceAttributes surface, DirectionalLight dir_light, float3 world_pos,
                                     Texture2D shadow_map, SamplerState shadow_sampler)
{
    float4 light_space_pos = mul(dir_light.shadow_matrix, float4(world_pos, 1.0));
    light_space_pos.xyz /= light_space_pos.w;
    float2 light_space_uv = light_space_pos.xy * 0.5f + 0.5f;
    float light_space_depth = light_space_pos.z - dir_light.normal_bias * abs(dot(dir_light.direction, surface.normal));
    float shadow_depth = shadow_map.Sample(shadow_sampler, light_space_uv).r + dir_light.depth_bias;
    bool in_directional_shadow = shadow_depth < light_space_depth;

    return in_directional_shadow;
}

#endif // SHADOW_H_