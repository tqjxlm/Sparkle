#ifndef DIRECTIONAL_LIGH_H_
#define DIRECTIONAL_LIGH_H_

struct DirectionalLight
{
    float3 color;
    float depth_bias;
    float3 direction;
    float normal_bias;
    float4x4 shadow_matrix;
};

#endif // DIRECTIONAL_LIGH_H_