#include "screen.h.hlsl"

#include "sky_light.h.hlsl"

[[vk::binding(0, 1)]] cbuffer ubo : register(b1)
{
    SkyLight ubo_sky_light;
}

float4 main(VSInterpolant input) : SV_Target
{
    const float t = 1.0 - input.uv.y;
    const float3 sky_light = lerp(float3(1, 1, 1), ubo_sky_light.color, t);

    return float4(sky_light, 1.0f);
}
