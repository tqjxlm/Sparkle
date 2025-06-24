#include "sky_map.h.hlsl"

[[vk::binding(0, 0)]] cbuffer view : register(b0)
{
    matrix ViewMatrix;
    matrix ProjectionMatrix;
}

struct VS_INPUT
{
    [[vk::location(0)]] float3 position : POSITION;
};

VSInterpolant main(VS_INPUT input)
{
    VSInterpolant output;
    const float unit_box_scale = 1.0f;

    float3 scaled_pos = input.position * unit_box_scale;
    output.local_pos = scaled_pos;
    output.gl_Position = mul(mul(ProjectionMatrix, ViewMatrix), float4(scaled_pos, 1.0f));

    output.gl_Position.z = output.gl_Position.w;

    return output;
}
