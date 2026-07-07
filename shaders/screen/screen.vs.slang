#include "screen.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    // columns of a 2x2 rotation, each padded to float4 (std140/MSL divergence for float2x2)
    float4 pre_rotation_col[2];
};

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSInterpolant main(VSInput input)
{
    VSInterpolant output;

    float2 rotatedPosition = pre_rotation_col[0].xy * input.position.x + pre_rotation_col[1].xy * input.position.y;
    output.position = float4(rotatedPosition, input.position.z, 1.0f);

    output.uv = input.uv;

    return output;
}
