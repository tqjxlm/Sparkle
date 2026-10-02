#include "color_slot.h.slang"
#include "sky_map.h.slang"

#include "math.h.slang"

[[vk::binding(0, 1)]] TextureCube sky_map;

[[vk::binding(1, 1)]] SamplerState sky_map_sampler;

[shader("fragment")] SceneColorOutput shader_main(VSInterpolant input) {
    float3 sky_light = sky_map.Sample(sky_map_sampler, normalize(input.local_pos)).rgb;

    return {float4(sky_light, 1.0f)};
}