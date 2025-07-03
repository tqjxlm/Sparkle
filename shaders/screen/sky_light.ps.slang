#include "screen.h.slang"

#include "sky_light.h.slang"

[[vk::binding(0, 1)]] cbuffer ubo
{
    SkyLight ubo_sky_light;
}

[shader("fragment")]
float4 main(VSInterpolant input) : SV_Target
{
    const float t = 1.0 - input.uv.y;
    const float3 sky_light = lerp(float3(1, 1, 1), ubo_sky_light.color, t);

    return float4(sky_light, 1.0f);
}