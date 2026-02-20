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
    uint history_cap;
};

[[vk::binding(1, 0)]] Texture2D<float4> noisyTexture;
[[vk::binding(2, 0)]] Texture2D<float4> featureNormalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> featureAlbedoMetallicTexture;
[[vk::binding(4, 0)]] Texture2D<float> featureDepthTexture;
[[vk::binding(5, 0)]] Texture2D<uint> featurePrimitiveIdTexture;
[[vk::binding(6, 0)]] Texture2D<float> reprojectionMaskTexture;
[[vk::binding(7, 0)]] Texture2D<float4> historyMomentsTexture;
[[vk::binding(8, 0)]] RWTexture2D<float4> outputImage;
[[vk::binding(9, 0)]] Texture2D<float4> reprojectionDebugTexture;

static const uint InvalidPrimitiveId = 0xffffffffu;

static const uint ASVGF_DEBUG_VIEW_NOISY = 1u;
static const uint ASVGF_DEBUG_VIEW_NORMAL = 2u;
static const uint ASVGF_DEBUG_VIEW_ALBEDO = 3u;
static const uint ASVGF_DEBUG_VIEW_DEPTH = 4u;
static const uint ASVGF_DEBUG_VIEW_REPROJECTION_MASK = 5u;
static const uint ASVGF_DEBUG_VIEW_HISTORY_LENGTH = 6u;
static const uint ASVGF_DEBUG_VIEW_MOMENTS = 7u;
static const uint ASVGF_DEBUG_VIEW_VARIANCE = 8u;
static const uint ASVGF_DEBUG_VIEW_FILTERED = 9u;
static const uint ASVGF_DEBUG_VIEW_REPROJECTION_REASON = 10u;
static const uint ASVGF_DEBUG_VIEW_REPROJECTION_UV = 11u;
static const uint ASVGF_DEBUG_VIEW_REPROJECTION_DEPTH_ERROR = 12u;
static const uint ASVGF_DEBUG_VIEW_QUADRANT = 13u;
static const uint ASVGF_DEBUG_VIEW_HISTORY_LENGTH_RAW = 14u;

static const uint ReprojectionReasonNoFeature = 0u;
static const uint ReprojectionReasonNoPreviousCamera = 1u;
static const uint ReprojectionReasonProjectionFailed = 2u;
static const uint ReprojectionReasonPrimitiveMismatch = 3u;
static const uint ReprojectionReasonDepthMismatch = 4u;
static const uint ReprojectionReasonNormalMismatch = 5u;
static const uint ReprojectionReasonValid = 6u;

float3 HashToColor(uint value)
{
    float3 color;
    color.r = ((value >> 0) & 255) / 255.0;
    color.g = ((value >> 8) & 255) / 255.0;
    color.b = ((value >> 16) & 255) / 255.0;
    return color;
}

float3 ReprojectionReasonColor(uint reason)
{
    switch (reason)
    {
    case ReprojectionReasonNoFeature:
        return float3(0.0, 0.0, 0.0);
    case ReprojectionReasonNoPreviousCamera:
        return float3(1.0, 0.5, 0.0);
    case ReprojectionReasonProjectionFailed:
        return float3(1.0, 0.0, 1.0);
    case ReprojectionReasonPrimitiveMismatch:
        return float3(1.0, 0.0, 0.0);
    case ReprojectionReasonDepthMismatch:
        return float3(1.0, 1.0, 0.0);
    case ReprojectionReasonNormalMismatch:
        return float3(0.0, 0.6, 1.0);
    case ReprojectionReasonValid:
        return float3(1.0, 1.0, 1.0);
    default:
        return float3(0.0, 1.0, 0.0);
    }
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
    case ASVGF_DEBUG_VIEW_REPROJECTION_MASK: {
        float mask = reprojectionMaskTexture.Load(texel);
        color = saturate(mask).xxx;
        break;
    }
    case ASVGF_DEBUG_VIEW_HISTORY_LENGTH: {
        float history_length = historyMomentsTexture.Load(texel).w;
        float history_cap_f = max((float)history_cap, 1.0);
        color = saturate(history_length / history_cap_f).xxx;
        break;
    }
    case ASVGF_DEBUG_VIEW_MOMENTS: {
        float4 moments = historyMomentsTexture.Load(texel);
        color = saturate(float3(moments.x, moments.y, moments.z));
        break;
    }
    case ASVGF_DEBUG_VIEW_REPROJECTION_REASON: {
        uint reason = (uint)round(reprojectionDebugTexture.Load(texel).x);
        color = ReprojectionReasonColor(reason);
        break;
    }
    case ASVGF_DEBUG_VIEW_REPROJECTION_UV: {
        float2 previous_uv = reprojectionDebugTexture.Load(texel).yz;
        bool uv_available = previous_uv.x >= 0.0 && previous_uv.y >= 0.0;
        if (!uv_available)
        {
            color = float3(1.0, 0.0, 1.0);
            break;
        }

        float2 clamped_uv = saturate(previous_uv);
        float2 outside_error = abs(previous_uv - clamped_uv);
        color = float3(clamped_uv, saturate(max(outside_error.x, outside_error.y) * 4.0));
        break;
    }
    case ASVGF_DEBUG_VIEW_REPROJECTION_DEPTH_ERROR: {
        float ratio = reprojectionDebugTexture.Load(texel).w;
        float ratio_vis = saturate(ratio);
        color = float3(ratio_vis, 1.0 - ratio_vis, 0.0);
        break;
    }
    case ASVGF_DEBUG_VIEW_QUADRANT: {
        bool right = pixel.x >= (resolution_x / 2u);
        bool bottom = pixel.y >= (resolution_y / 2u);
        if (!right && !bottom)
        {
            color = float3(1.0, 0.2, 0.2);
        }
        else if (right && !bottom)
        {
            color = float3(0.2, 1.0, 0.2);
        }
        else if (!right && bottom)
        {
            color = float3(0.2, 0.2, 1.0);
        }
        else
        {
            color = float3(1.0, 1.0, 0.2);
        }
        break;
    }
    case ASVGF_DEBUG_VIEW_HISTORY_LENGTH_RAW: {
        float history_length = historyMomentsTexture.Load(texel).w;
        float history_cap_f = max((float)history_cap, 1.0);
        float history_norm = saturate(history_length / history_cap_f);
        color = float3(history_norm, frac(history_length * 0.25), frac(history_length * 0.0625));
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
