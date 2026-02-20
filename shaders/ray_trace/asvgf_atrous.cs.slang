[[vk::binding(0, 0)]] cbuffer ubo
{
    uint resolution_x;
    uint resolution_y;
    uint step_width;
    uint iteration_index;
    float color_sigma_scale;
    float depth_sigma_scale;
    float normal_power;
    float albedo_sigma;
};

[[vk::binding(1, 0)]] Texture2D<float4> inputColorTexture;
[[vk::binding(2, 0)]] Texture2D<float4> featureNormalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> featureAlbedoMetallicTexture;
[[vk::binding(4, 0)]] Texture2D<float> featureDepthTexture;
[[vk::binding(5, 0)]] Texture2D<uint> featurePrimitiveIdTexture;
[[vk::binding(6, 0)]] Texture2D<float> varianceTexture;
[[vk::binding(7, 0)]] RWTexture2D<float4> outFilteredImage;

static const uint InvalidPrimitiveId = 0xffffffffu;
static const float3 LuminanceWeights = float3(0.2126, 0.7152, 0.0722);
static const float AtrousKernel1D[5] = {1.0, 4.0, 6.0, 4.0, 1.0};

float3 SafeNormalize(float3 vector_value)
{
    float length_sqr = dot(vector_value, vector_value);
    if (length_sqr <= 1e-8)
    {
        return float3(0.0, 0.0, 1.0);
    }

    return vector_value * rsqrt(length_sqr);
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
    float3 center_color = max(inputColorTexture.Load(center_texel).rgb, 0.0);
    float center_depth = featureDepthTexture.Load(center_texel);
    uint center_primitive_id = featurePrimitiveIdTexture.Load(center_texel);
    bool center_feature_valid = center_primitive_id != InvalidPrimitiveId && center_depth > 0.0;
    if (!center_feature_valid)
    {
        outFilteredImage[pixel] = float4(center_color, 1.0);
        return;
    }

    float3 center_normal = SafeNormalize(featureNormalRoughnessTexture.Load(center_texel).xyz);
    float3 center_albedo = saturate(featureAlbedoMetallicTexture.Load(center_texel).rgb);
    float center_variance = max(varianceTexture.Load(center_texel), 0.0);
    float center_luma = dot(center_color, LuminanceWeights);
    float color_sigma_base = color_sigma_scale * (1.0 + 0.8 * (float)iteration_index);

    int2 max_pixel = int2(int(resolution_x) - 1, int(resolution_y) - 1);
    int step = int(max(step_width, 1u));

    float3 weighted_color_sum = 0.0;
    float weight_sum = 0.0;

    [unroll] for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            int2 sample_pixel = int2(pixel) + int2(dx, dy) * step;
            if (sample_pixel.x < 0 || sample_pixel.y < 0 || sample_pixel.x > max_pixel.x || sample_pixel.y > max_pixel.y)
            {
                continue;
            }
            int3 sample_texel = int3(sample_pixel, 0);

            float sample_depth = featureDepthTexture.Load(sample_texel);
            uint sample_primitive_id = featurePrimitiveIdTexture.Load(sample_texel);
            bool sample_feature_valid = sample_primitive_id != InvalidPrimitiveId && sample_depth > 0.0;
            if (!sample_feature_valid)
            {
                continue;
            }

            float3 sample_color = max(inputColorTexture.Load(sample_texel).rgb, 0.0);
            float3 sample_normal = SafeNormalize(featureNormalRoughnessTexture.Load(sample_texel).xyz);
            float3 sample_albedo = saturate(featureAlbedoMetallicTexture.Load(sample_texel).rgb);
            float sample_variance = max(varianceTexture.Load(sample_texel), 0.0);
            float sample_luma = dot(sample_color, LuminanceWeights);

            float kernel_weight = AtrousKernel1D[dx + 2] * AtrousKernel1D[dy + 2];
            float primitive_weight = sample_primitive_id == center_primitive_id ? 1.0 : 0.15;

            float normal_similarity = saturate(dot(center_normal, sample_normal));
            float normal_weight = pow(normal_similarity, normal_power);

            float depth_sigma = max(max(center_depth, sample_depth) * depth_sigma_scale * (float(step) + 1.0), 1e-3);
            float depth_weight = exp(-abs(sample_depth - center_depth) / depth_sigma);

            float albedo_diff = length(sample_albedo - center_albedo);
            float albedo_weight = exp(-albedo_diff / max(albedo_sigma, 1e-3));

            float color_sigma =
                max(color_sigma_base * sqrt(max(center_variance, sample_variance) + 1e-6), 1e-3);
            float color_weight = exp(-abs(sample_luma - center_luma) / color_sigma);

            float sample_weight = kernel_weight * primitive_weight * normal_weight * depth_weight * albedo_weight * color_weight;
            weighted_color_sum += sample_color * sample_weight;
            weight_sum += sample_weight;
        }
    }

    float3 filtered_color = center_color;
    if (weight_sum > 1e-6)
    {
        filtered_color = weighted_color_sum / weight_sum;
    }

    outFilteredImage[pixel] = float4(max(filtered_color, 0.0), 1.0);
}
