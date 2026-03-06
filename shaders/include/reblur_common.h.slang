#ifndef REBLUR_COMMON_H
#define REBLUR_COMMON_H

struct HitDistanceParams
{
    float A;
    float B;
    float C;
    float D;
};

float NormalizeHitDistance(float hit_dist, float view_z, float roughness, HitDistanceParams params)
{
    float scale = (params.A + abs(view_z) * params.B) * lerp(1.0, params.C, exp2(params.D * roughness * roughness));
    return saturate(hit_dist / max(scale, 1e-6));
}

float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * select(v.xy >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
}

float2 EncodeNormalOct(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

float3 DecodeNormalOct(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, float2(-t, -t), float2(t, t));
    return normalize(n);
}

float GetLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// NRD's specular magic curve: controls how roughness affects specular blur/weights
// Returns ~0 for mirror-like surfaces, ~1 for rough surfaces
float GetSpecMagicCurve(float roughness)
{
    return (1.0 - exp2(-200.0 * roughness * roughness)) * pow(roughness, 0.25);
}

// Denormalize hit distance back to world-space units
float DenormalizeHitDistance(float norm_hit_dist, float view_z, float roughness, HitDistanceParams params)
{
    float scale = (params.A + abs(view_z) * params.B) * lerp(1.0, params.C, exp2(params.D * roughness * roughness));
    return norm_hit_dist * scale;
}

// Frustum size at given depth: world-space extent of minimum screen dimension
float GetFrustumSize(float min_rect_dim_mul_unproject, float view_z)
{
    return min_rect_dim_mul_unproject * abs(view_z);
}

// Hit distance factor normalized by frustum size (screen-space relative)
float GetHitDistFactor(float hit_dist, float frustum_size)
{
    return saturate(hit_dist / max(frustum_size, 1e-6));
}

// NRD-style hit-distance weight parameters for spatial filtering.
// Using normalized hit distance keeps the rejection behavior scene-scale
// invariant and makes the kernel much stricter once stable history exists.
float2 GetHitDistanceWeightParams(float hit_dist, float non_linear_accum_speed, float roughness = 1.0)
{
    float smc = GetSpecMagicCurve(roughness);
    float norm = lerp(0.0005, 1.0, min(non_linear_accum_speed, smc));
    float a = 1.0 / max(norm, 1e-6);
    float b = hit_dist * a;
    return float2(a, -b);
}

float ComputeWeight(float x, float px, float py)
{
    return smoothstep(1.0, 0.0, abs(x * px + py));
}

// Specular lobe tangent half-angle (approximation of GGX lobe extent)
float GetSpecularLobeTanHalfAngle(float roughness, float percent_of_volume)
{
    float m = roughness * roughness;
    return m * percent_of_volume / (1.0 + (1.0 - percent_of_volume) * m * m);
}

// Roughness weight for specular bilateral filtering
float GetRoughnessWeight(float center_roughness, float sample_roughness, float fraction)
{
    float a = 1.0 / max(lerp(0.01, 1.0, saturate(fraction * center_roughness * center_roughness)), 1e-6);
    float b = center_roughness * a;
    return smoothstep(1.0, 0.0, abs(sample_roughness * a - b));
}

// Linear RGB to YCoCg color space
float3 LinearToYCoCg(float3 color)
{
    float Y  = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

// YCoCg to linear RGB color space
float3 YCoCgToLinear(float3 ycocg)
{
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

// Change luminance of a color proportionally
float3 ChangeLuma(float3 color, float new_luma)
{
    float old_luma = GetLuminance(color);
    return color * (new_luma / max(old_luma, 1e-6));
}

#endif // REBLUR_COMMON_H
