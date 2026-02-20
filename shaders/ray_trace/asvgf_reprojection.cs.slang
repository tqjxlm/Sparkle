[[vk::binding(0, 0)]] cbuffer ubo
{
    float4 current_camera_position;
    float4 previous_camera_position;
    float4 current_lower_left;
    float4 current_max_u;
    float4 current_max_v;
    float4 previous_lower_left;
    float4 previous_max_u;
    float4 previous_max_v;
    uint resolution_x;
    uint resolution_y;
    uint history_cap;
    uint has_previous_camera;
};

[[vk::binding(1, 0)]] Texture2D<float4> noisyTexture;
[[vk::binding(2, 0)]] Texture2D<float4> featureNormalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float> featureDepthTexture;
[[vk::binding(4, 0)]] Texture2D<uint> featurePrimitiveIdTexture;
[[vk::binding(5, 0)]] Texture2D<float4> prevHistoryColorTexture;
[[vk::binding(6, 0)]] Texture2D<float4> prevHistoryMomentsTexture;
[[vk::binding(7, 0)]] Texture2D<float4> prevFeatureNormalRoughnessTexture;
[[vk::binding(8, 0)]] Texture2D<float> prevFeatureDepthTexture;
[[vk::binding(9, 0)]] Texture2D<uint> prevFeaturePrimitiveIdTexture;
[[vk::binding(10, 0)]] RWTexture2D<float4> outHistoryColorImage;
[[vk::binding(11, 0)]] RWTexture2D<float4> outHistoryMomentsImage;
[[vk::binding(12, 0)]] RWTexture2D<float4> outFeatureNormalRoughnessImage;
[[vk::binding(13, 0)]] RWTexture2D<float> outFeatureDepthImage;
[[vk::binding(14, 0)]] RWTexture2D<uint> outFeaturePrimitiveIdImage;
[[vk::binding(15, 0)]] RWTexture2D<float> outReprojectionMaskImage;
[[vk::binding(16, 0)]] RWTexture2D<float4> outReprojectionDebugImage;

static const uint InvalidPrimitiveId = 0xffffffffu;
static const float3 LuminanceWeights = float3(0.2126, 0.7152, 0.0722);
static const uint ReprojectionReasonNoFeature = 0u;
static const uint ReprojectionReasonNoPreviousCamera = 1u;
static const uint ReprojectionReasonProjectionFailed = 2u;
static const uint ReprojectionReasonPrimitiveMismatch = 3u;
static const uint ReprojectionReasonDepthMismatch = 4u;
static const uint ReprojectionReasonNormalMismatch = 5u;
static const uint ReprojectionReasonValid = 6u;

float3 ComputeWorldPosition(uint2 pixel, float depth)
{
    float2 denominator = float2(max(resolution_x - 1u, 1u), max(resolution_y - 1u, 1u));
    float2 uv = (float2(pixel) + 0.5) / denominator;
    uv.y = 1.0 - uv.y;

    float3 ray_direction =
        normalize(current_lower_left.xyz + uv.x * current_max_u.xyz + uv.y * current_max_v.xyz - current_camera_position.xyz);
    return current_camera_position.xyz + ray_direction * depth;
}

bool ReprojectToPreviousPixel(float3 world_position, out uint2 previous_pixel, out float2 previous_uv)
{
    previous_pixel = 0;
    previous_uv = float2(-1.0, -1.0);

    float3 to_world = world_position - previous_camera_position.xyz;
    float3 plane_normal = normalize(cross(previous_max_v.xyz, previous_max_u.xyz));

    float denominator = dot(plane_normal, to_world);
    if (abs(denominator) <= 1e-6)
    {
        return false;
    }

    float focus_t = dot(plane_normal, previous_lower_left.xyz - previous_camera_position.xyz) / denominator;
    if (focus_t <= 0.0)
    {
        return false;
    }

    float3 focus_hit = previous_camera_position.xyz + to_world * focus_t;
    float3 focus_offset = focus_hit - previous_lower_left.xyz;

    float max_u_length_sqr = dot(previous_max_u.xyz, previous_max_u.xyz);
    float max_v_length_sqr = dot(previous_max_v.xyz, previous_max_v.xyz);
    if (max_u_length_sqr <= 1e-6 || max_v_length_sqr <= 1e-6)
    {
        return false;
    }

    float u = dot(focus_offset, previous_max_u.xyz) / max_u_length_sqr;
    float v = dot(focus_offset, previous_max_v.xyz) / max_v_length_sqr;

    if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0)
    {
        previous_uv = float2(u, 1.0 - v);
        return false;
    }

    previous_uv = float2(u, 1.0 - v);
    float2 prev_pixel_f = previous_uv * float2(max(resolution_x - 1u, 1u), max(resolution_y - 1u, 1u));
    previous_pixel = uint2(prev_pixel_f);
    previous_pixel = min(previous_pixel, uint2(resolution_x - 1u, resolution_y - 1u));

    return true;
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution_x || pixel.y >= resolution_y)
    {
        return;
    }

    int3 texel = int3(pixel, 0);
    float4 noisy = noisyTexture.Load(texel);
    float3 noisy_color = max(noisy.rgb, 0.0);

    float current_depth = featureDepthTexture.Load(texel);
    uint current_primitive_id = featurePrimitiveIdTexture.Load(texel);
    float4 current_normal_roughness = featureNormalRoughnessTexture.Load(texel);

    bool feature_valid = current_primitive_id != InvalidPrimitiveId && current_depth > 0.0;

    float reprojected_mask = 0.0;
    float history_length = feature_valid ? 1.0 : 0.0;
    float3 reprojected_color = noisy_color;
    float2 reprojected_moments = float2(dot(noisy_color, LuminanceWeights), 0.0);
    reprojected_moments.y = reprojected_moments.x * reprojected_moments.x;
    uint reprojection_reason = feature_valid ? ReprojectionReasonNoPreviousCamera : ReprojectionReasonNoFeature;
    float2 reprojected_uv = float2(-1.0, -1.0);
    float depth_error_ratio = 0.0;

    if (feature_valid && has_previous_camera != 0u)
    {
        reprojection_reason = ReprojectionReasonProjectionFailed;
        float3 world_position = ComputeWorldPosition(pixel, current_depth);
        uint2 previous_pixel = 0;
        if (ReprojectToPreviousPixel(world_position, previous_pixel, reprojected_uv))
        {
            int3 previous_texel = int3(previous_pixel, 0);

            uint previous_primitive_id = prevFeaturePrimitiveIdTexture.Load(previous_texel);
            float previous_depth = prevFeatureDepthTexture.Load(previous_texel);
            float3 previous_normal = normalize(prevFeatureNormalRoughnessTexture.Load(previous_texel).xyz);
            float3 current_normal = normalize(current_normal_roughness.xyz);

            float expected_previous_depth = length(world_position - previous_camera_position.xyz);
            float depth_threshold = max(0.05, expected_previous_depth * 0.02);
            float depth_error = abs(previous_depth - expected_previous_depth);
            depth_error_ratio = depth_error / max(depth_threshold, 1e-6);

            bool primitive_valid = previous_primitive_id == current_primitive_id && previous_primitive_id != InvalidPrimitiveId;
            bool depth_valid = depth_error <= depth_threshold;
            bool normal_valid = dot(previous_normal, current_normal) > 0.75;

            if (primitive_valid && depth_valid && normal_valid)
            {
                reprojected_mask = 1.0;
                reprojection_reason = ReprojectionReasonValid;
                reprojected_color = max(prevHistoryColorTexture.Load(previous_texel).rgb, 0.0);

                float4 previous_moments = prevHistoryMomentsTexture.Load(previous_texel);
                reprojected_moments = previous_moments.xy;
                history_length = min(previous_moments.w + 1.0, (float)history_cap);
            }
            else if (!primitive_valid)
            {
                reprojection_reason = ReprojectionReasonPrimitiveMismatch;
            }
            else if (!depth_valid)
            {
                reprojection_reason = ReprojectionReasonDepthMismatch;
            }
            else
            {
                reprojection_reason = ReprojectionReasonNormalMismatch;
            }
        }
    }
    else if (feature_valid && has_previous_camera == 0u)
    {
        reprojection_reason = ReprojectionReasonNoPreviousCamera;
    }

    outHistoryColorImage[pixel] = float4(reprojected_color, 1.0);
    outHistoryMomentsImage[pixel] = float4(reprojected_moments, reprojected_mask, history_length);
    outFeatureNormalRoughnessImage[pixel] = current_normal_roughness;
    outFeatureDepthImage[pixel] = current_depth;
    outFeaturePrimitiveIdImage[pixel] = current_primitive_id;
    outReprojectionMaskImage[pixel] = reprojected_mask;
    outReprojectionDebugImage[pixel] = float4((float)reprojection_reason, reprojected_uv, depth_error_ratio);
}
