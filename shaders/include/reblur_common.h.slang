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

#endif // REBLUR_COMMON_H
