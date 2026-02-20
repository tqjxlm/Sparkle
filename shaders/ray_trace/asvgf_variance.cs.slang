[[vk::binding(0, 0)]] cbuffer ubo
{
    uint resolution_x;
    uint resolution_y;
    float depth_sigma_scale;
    float normal_power;
};

[[vk::binding(1, 0)]] Texture2D<float4> historyMomentsTexture;
[[vk::binding(2, 0)]] Texture2D<float4> featureNormalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float> featureDepthTexture;
[[vk::binding(4, 0)]] RWTexture2D<float> outVarianceImage;

float3 SafeNormalize(float3 vector_value)
{
    float length_sqr = dot(vector_value, vector_value);
    if (length_sqr <= 1e-8)
    {
        return float3(0.0, 0.0, 1.0);
    }

    return vector_value * rsqrt(length_sqr);
}

float VarianceFromMoments(float2 moments)
{
    return max(moments.y - moments.x * moments.x, 0.0);
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution_x || pixel.y >= resolution_y)
    {
        return;
    }

    int3 center_texel = int3(pixel, 0);
    float4 center_moments = historyMomentsTexture.Load(center_texel);
    float center_history_length = center_moments.w;
    float center_depth = featureDepthTexture.Load(center_texel);
    float3 center_normal = SafeNormalize(featureNormalRoughnessTexture.Load(center_texel).xyz);

    float center_variance = VarianceFromMoments(center_moments.xy);
    float stabilized_variance = center_variance;

    if (center_history_length > 0.0 && center_depth > 0.0)
    {
        float weighted_sum = center_variance;
        float weight_sum = 1.0;
        int2 max_pixel = int2(int(resolution_x) - 1, int(resolution_y) - 1);

        [unroll] for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                {
                    continue;
                }

                int2 sample_pixel = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), max_pixel);
                int3 sample_texel = int3(sample_pixel, 0);
                float4 sample_moments = historyMomentsTexture.Load(sample_texel);
                float sample_history_length = sample_moments.w;
                float sample_depth = featureDepthTexture.Load(sample_texel);
                if (sample_history_length <= 0.0 || sample_depth <= 0.0)
                {
                    continue;
                }

                float3 sample_normal = SafeNormalize(featureNormalRoughnessTexture.Load(sample_texel).xyz);
                float sample_variance = VarianceFromMoments(sample_moments.xy);

                float normal_similarity = saturate(dot(center_normal, sample_normal));
                float normal_weight = pow(normal_similarity, normal_power);

                float depth_sigma = max(max(center_depth, sample_depth) * depth_sigma_scale, 1e-3);
                float depth_weight = exp(-abs(sample_depth - center_depth) / depth_sigma);

                float weight = normal_weight * depth_weight;
                weighted_sum += sample_variance * weight;
                weight_sum += weight;
            }
        }

        stabilized_variance = weighted_sum / max(weight_sum, 1e-6);
    }

    outVarianceImage[pixel] = max(min(stabilized_variance, 1e4), 0.0);
}
