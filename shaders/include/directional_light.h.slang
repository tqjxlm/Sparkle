#ifndef DIRECTIONAL_LIGH_H_
#define DIRECTIONAL_LIGH_H_

// field order is layout-critical: a float3 must not be followed by a scalar (std140/MSL divergence)
struct DirectionalLight
{
    float depth_bias;
    float normal_bias;
    float3 color;
    float3 direction;
    float4x4 shadow_matrix;
};

#endif // DIRECTIONAL_LIGH_H_