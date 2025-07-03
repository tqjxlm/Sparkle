#ifndef DEPTH_H_
#define DEPTH_H_

#include "debug.h.slang"
#include "view.h.slang"

float3 GetWorldPositionFromDepth(float depth, float2 uv, View view)
{
    float4 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;
    ndc.z = depth;
    ndc.w = 1.0;

    float4 view_space = mul(view.InvProjectionMatrix, ndc);
    view_space /= view_space.w;

    view_space.y = -view_space.y;

    float4 world_space = mul(view.InvViewMatrix, view_space);
    return world_space.xyz;
}

float LinearizeDepth(float depth, View view)
{
    return (view.near * view.far) / (view.far + view.near - depth * (view.far - view.near));
}

float3 VisualizeDepth(float depth, View view)
{
    float linear_depth = LinearizeDepth(depth, view);
    return ((float3)1) * linear_depth / view.far;
}

#endif