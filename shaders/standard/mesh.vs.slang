#include "mesh.h.slang"
#include "standard.h.slang"
#include "view.h.slang"

struct VS_INPUT
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 tangent : TANGENT;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] cbuffer view
{
    View view;
}

[[vk::binding(0, 1)]] cbuffer mesh
{
    matrix ModelMatrix;
    matrix ModelMatrixInvTranspose;
}

[shader("vertex")]
MeshPassVSInterpolant main(VS_INPUT input)
{
    MeshPassVSInterpolant output;

    float4 world_pos = mul(ModelMatrix, float4(input.position.xyz, 1.0f));

    float4 clip_pos = mul(view.ViewProjectionMatrix, world_pos);

    output.gl_Position = clip_pos;

    output.tangent = input.tangent.xyz;
    output.bi_tangent = cross(input.normal, input.tangent.xyz) * input.tangent.w;
    output.normal = input.normal;
    output.world_pos = world_pos.xyz;
    output.uv = input.uv;

    return output;
}