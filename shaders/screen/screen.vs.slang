#include "screen.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    float2x2 pre_rotation;
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

    float2 rotatedPosition = mul(pre_rotation, input.position.xy);
    output.position = float4(rotatedPosition, input.position.z, 1.0f);

    output.uv = input.uv;

    return output;
}
