[[vk::binding(0, 0)]] cbuffer ubo
{
    uint stage;
    uint view;
    uint history_index;
    uint frame_index;
    uint resolution_x;
    uint resolution_y;
    uint freeze_history;
    uint force_clear_history;
};

[[vk::binding(1, 0)]] Texture2D<float4> noisyTexture;
[[vk::binding(2, 0)]] Texture2D<float4> featureNormalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> featureAlbedoMetallicTexture;
[[vk::binding(4, 0)]] Texture2D<float> featureDepthTexture;
[[vk::binding(5, 0)]] Texture2D<uint> featurePrimitiveIdTexture;
[[vk::binding(6, 0)]] RWTexture2D<float4> outputImage;

static const uint InvalidPrimitiveId = 0xffffffffu;

static const uint ASVGF_DEBUG_VIEW_NOISY = 1u;
static const uint ASVGF_DEBUG_VIEW_NORMAL = 2u;
static const uint ASVGF_DEBUG_VIEW_ALBEDO = 3u;
static const uint ASVGF_DEBUG_VIEW_DEPTH = 4u;

float3 HashToColor(uint value)
{
    float3 color;
    color.r = ((value >> 0) & 255) / 255.0;
    color.g = ((value >> 8) & 255) / 255.0;
    color.b = ((value >> 16) & 255) / 255.0;
    return color;
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution_x || pixel.y >= resolution_y)
    {
        return;
    }

    float3 color = 0.0;
    int3 texel = int3(pixel.x, pixel.y, 0);
    uint primitive_id = featurePrimitiveIdTexture.Load(texel);
    bool feature_valid = primitive_id != InvalidPrimitiveId;

    switch (view)
    {
    case ASVGF_DEBUG_VIEW_NOISY:
        color = max(noisyTexture.Load(texel).rgb, 0.0);
        break;
    case ASVGF_DEBUG_VIEW_NORMAL: {
        float3 normal = featureNormalRoughnessTexture.Load(texel).xyz;
        color = feature_valid ? saturate(normal * 0.5 + 0.5) : 0.0;
        break;
    }
    case ASVGF_DEBUG_VIEW_ALBEDO:
        color = feature_valid ? saturate(featureAlbedoMetallicTexture.Load(texel).rgb) : 0.0;
        break;
    case ASVGF_DEBUG_VIEW_DEPTH: {
        float depth = featureDepthTexture.Load(texel);
        float depth_vis = feature_valid ? saturate(1.0 / (1.0 + depth * 0.1)) : 0.0;
        color = depth_vis.xxx;
        break;
    }
    default: {
        uint hash_value = (stage + 1u) * 73856093u;
        hash_value ^= (view + 1u) * 19349663u;
        hash_value ^= (history_index + 1u) * 83492791u;

        float3 base = HashToColor(hash_value);
        float checker = (((pixel.x / 16u) + (pixel.y / 16u)) & 1u) == 0u ? 1.0 : 0.72;
        float pulse = (frame_index & 1u) == 0u ? 1.0 : 0.92;
        color = saturate(base * checker * pulse);
        break;
    }
    }

    if (freeze_history != 0u)
    {
        color = saturate(color + float3(0.15, 0.0, 0.0));
    }

    if (force_clear_history != 0u)
    {
        color = saturate(color + float3(0.0, 0.15, 0.0));
    }

    outputImage[pixel] = float4(color, 1.0);
}
