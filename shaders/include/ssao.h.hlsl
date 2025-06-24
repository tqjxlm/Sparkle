#ifndef SSAO_H_
#define SSAO_H_

#include "math.h.hlsl"

const static int ssao_samples = 16;

struct SSAOConfig
{
    float4 samples[ssao_samples];
    float scale;
    float threashold;
};

float CalculateSSAO(float3 world_pos, float4x4 view_projection_matrix, float3x3 model_matrix_inv_transpose,
                    SSAOConfig ssao_config, Texture2D prepass_depth_map, SamplerState prepass_depth_map_sampler,
                    float near, float far)
{
    float occluded[ssao_samples];
    for (int i = 0; i < ssao_samples; i++)
    {
        float3 sample_direction = normalize(mul(model_matrix_inv_transpose, (float3)ssao_config.samples[i].xyz));
        float3 sample_pos = world_pos + sample_direction * ssao_config.scale;

        float4 sample_point = mul(view_projection_matrix, float4(sample_pos, 1.f));
        sample_point.xyz /= sample_point.w;
        sample_point.xy = sample_point.xy * 0.5f + 0.5f;

        float depth = prepass_depth_map.Sample(prepass_depth_map_sampler, sample_point.xy).r;
        float linear_z = ToLinearDepth(sample_point.z, near, far);
        float linear_depth = ToLinearDepth(depth, near, far);

        occluded[i] = (linear_z > linear_depth) && (linear_z - linear_depth < ssao_config.threashold) ? 1.0f : 0.0f;
    }

    float occluded_cnt = 0;
    for (int i = 0; i < ssao_samples; i++)
    {
        occluded_cnt += occluded[i];
    }

    return 1.f - occluded_cnt / ssao_samples;
}

#endif // SSAO_H_
