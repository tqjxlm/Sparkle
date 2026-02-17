#ifndef MATH_H_
#define MATH_H_

static const float Pi = 3.1415926535897932385f;
static const float InvPi = 0.3183099f;
static const float InvSqrt3 = 0.57735026919f;
static const float Eps = 1e-6f;
static const float Tolerance = 1e-3f;

float SaturateDot(float3 a, float3 b)
{
    return max(dot(a, b), 0);
}

float3 TransformPoint(float3 p, float4x3 m)
{
    return mul(transpose(m), float4(p, 1));
}

float3 TransformDirection(float3 v, float3x3 m)
{
    return normalize(mul(m, v));
}

float4 LinearToSRGB(float4 linearRGB)
{
    bool3 cutoff = linearRGB.rgb < (float3)0.0031308;
    float3 higher = (float3)1.055 * pow(linearRGB.rgb, 1.0 / 2.4) - 0.055;
    float3 lower = linearRGB.rgb * 12.92;

    return float4(select(cutoff, lower, higher), linearRGB.a);
}

float3 LinearToSRGB(float3 linearRGB)
{
    bool3 cutoff = linearRGB.rgb < (float3)0.0031308;
    float3 higher = (float3)1.055 * pow(linearRGB.rgb, 1.0 / 2.4) - 0.055;
    float3 lower = linearRGB.rgb * 12.92;

    return select(cutoff, lower, higher);
}

float4 SRGBtoLinear(float4 sRGB)
{
    bool3 cutoff = sRGB.rgb < (float3)0.04045;
    float3 higher = pow((sRGB.rgb + 0.055) / 1.055, 2.4);
    float3 lower = sRGB.rgb / 12.92;

    return float4(select(cutoff, lower, higher), sRGB.a);
}

float3 SRGBtoLinear(float3 sRGB)
{
    bool3 cutoff = sRGB.rgb < (float3)0.04045;
    float3 higher = pow((sRGB.rgb + 0.055) / 1.055, 2.4);
    float3 lower = sRGB.rgb / 12.92;

    return select(cutoff, lower, higher);
}

float3 ClampToLength(float3 v, float max_length)
{
    const float length_v = length(v);
    return (length_v > max_length) ? v * (max_length / length_v) : v;
}

float ToLinearDepth(float depth, float near, float far)
{
    return near * far / (far - depth * (far - near));
}

float3 GetPossibleMajorAxis(float3 normal)
{
    if (abs(normal.x) < InvSqrt3)
    {
        return float3(1.0f, 0.0f, 0.0f);
    }
    if (abs(normal.y) < InvSqrt3)
    {
        return float3(0.0f, 1.0f, 0.0f);
    }

    return float3(0.0f, 0.0f, 1.0f);
}

void GetLocalAxisFromNormal(float3 normal, float3 major_axis, out float3 u, out float3 v, out float3 w)
{
    // Use majorAxis to create a coordinate system relative to world space
    u = normalize(cross(normal, major_axis));
    v = cross(normal, u);
    w = normal;
}

void GetLocalAxisFromNormal(float3 normal, out float3 u, out float3 v, out float3 w)
{
    // Find an axis that is not parallel to normal
    const float3 major_axis = GetPossibleMajorAxis(normal);

    // Use majorAxis to create a coordinate system relative to world space
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);
}

float3 TransformBasisToWorld(float3 dir, float3 u, float3 v, float3 w)
{
    float3x3 uvw = transpose(float3x3(u, v, w));
    return mul(uvw, dir);
}

float3 TransformBasisToLocal(float3 dir, float3 u, float3 v, float3 w)
{
    float3x3 uvw = transpose(float3x3(u, v, w));
    return mul(dir, uvw);
}

float3 TransformBasisToWorld(float3 dir, float3 normal, float3 major_axis)
{
    float3 u;
    float3 v;
    float3 w;
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);

    return TransformBasisToWorld(dir, u, v, w);
}

float3 TransformBasisToLocal(float3 dir, float3 normal, float3 major_axis)
{
    float3 u;
    float3 v;
    float3 w;
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);

    return TransformBasisToLocal(dir, u, v, w);
}

float2 CartesianToEquirectangular(float3 v)
{
    static const float2 invAtan = float2(0.1591f, 0.3183f);

    float2 uv = float2(atan2(v.y, v.x), asin(-v.z));
    uv *= invAtan;
    uv += 0.5f;
    return uv;
}

float2 EquirectangularToSpherical(float2 uv)
{
    // theta: [0, Pi]
    // phi: [-Pi, Pi]
    return float2(uv.y * Pi, uv.x * 2.0f * Pi - Pi);
}

float3 SphericalToCartesian(float cos_theta, float sin_theta, float cos_phi, float sin_phi)
{
    return float3(cos_phi * sin_theta, sin_phi * sin_theta, cos_theta);
}

// this function returns a unit vector
float3 SphericalToCartesian(float theta, float phi)
{
    return SphericalToCartesian(cos(theta), sin(theta), cos(phi), sin(phi));
}

float3 EquirectangularToCartesian(float2 uv)
{
    float2 sp = EquirectangularToSpherical(uv);
    return SphericalToCartesian(sp.x, sp.y);
}

uint GetBitMask(uint bits)
{
    return (1U << bits) - 1;
}

uint Float2UNorm(float value, uint take_up_bits)
{
    const uint max_value = GetBitMask(take_up_bits);
    return uint(round(value * max_value));
}

#endif // MATH_H_