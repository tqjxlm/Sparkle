#pragma once

#include "core/Exception.h"
#include "core/Logger.h"
#include "core/math/Types.h"

#include <format>

#define ARRAY_SIZE(array) ((array).size() * sizeof((array)[0]))

#define ARRAY_COUNT(array) (sizeof(array) / sizeof(array)[0])

namespace sparkle
{
namespace utilities
{
template <class T> inline std::string MatrixToString(const T &matrix)
{
    std::stringstream ss; // NOLINT
    ss << matrix;
    return ss.str();
}

template <typename T> std::string VectorToString(const T &v)
{
    std::stringstream ss; // NOLINT
    ss << '[' << v[0];
    for (int i = 1; i < T::SizeAtCompileTime; i++)
    {
        ss << ", " << v[i];
    }
    ss << ']';
    return ss.str();
}

inline std::string RotationToString(const Rotation &r)
{
    return std::format("[{:.6f}, {:.6f}, {:.6f}, {:.6f})]", r.x(), r.y(), r.z(), r.w());
}

template <class T> inline void Swap(T &a, T &b)
{
    T c = a;
    a = b;
    b = c;
}

template <class T> inline void PrintVector(const T &v)
{
    Log(Info, "[{}]", MatrixToString(v.transpose()));
}

inline float Degree2Radiance(float degrees)
{
    return degrees * Pi / 180.0f;
}

inline Vector3 ConcatVector(const Vector2 &v, Scalar value)
{
    return Vector3(v.x(), v.y(), value);
}

inline Vector4 ConcatVector(const Vector3 &v, Scalar value)
{
    return Vector4(v.x(), v.y(), v.z(), value);
}

template <class T> inline auto Clamp(const T &matrix, Scalar min, Scalar max)
{
    return matrix.cwiseMin(max).cwiseMax(min);
}

template <class T> inline T ToRadian(const T &degree)
{
    return degree / 180.f * Pi;
}

template <class T> inline T ToDegree(const T &radian)
{
    return radian / Pi * 180.f;
}

inline bool NearlyEqual(float x, float y)
{
    return std::abs(x - y) < Eps;
}

inline Vector3 VisualizeVector(const Vector3 &v)
{
    return (v + Ones) * 0.5f;
}

inline Vector3 VisualizeInteger(unsigned number)
{
    if (number >= 7)
    {
        return Vector3(0, 0, 0); // Black for out of bounds
    }

    // High contrast colors from https://www.schemecolor.com/high-contrast.php
    Vector3 palette[7] = {
        {137, 49, 239}, // Purple
        {242, 202, 25}, // Yellow
        {255, 0, 189},  // Magenta
        {0, 87, 233},   // Blue
        {135, 233, 17}, // Green
        {225, 24, 69},  // Red
        {255, 255, 255} // White
    };

    return palette[number] / 255.0f;
}

template <class T> T Lerp(const T &v0, const T &v1, const T &v2, Scalar u, Scalar v)
{
    return v0 + (v1 - v0) * u + (v2 - v0) * v;
}

template <class T> T Lerp(const T &a, const T &b, Scalar f)
{
    return a + (b - a) * f;
}

inline void Decompose(float f, int &integer, float &decimal)
{
    integer = static_cast<int>(f);
    decimal = f - static_cast<float>(integer);
}

inline Vector3 VisualizeVectorLength(const Vector3 &v)
{
    return Ones * v.squaredNorm();
}

inline Scalar CosTheta(const Vector3 &w)
{
    return w.z();
}

inline Scalar Cos2Theta(const Vector3 &w)
{
    return w.z() * w.z();
}

inline Scalar Sin2Theta(const Vector3 &w)
{
    return w.x() * w.x() + w.y() * w.y();
}

inline Scalar SinTheta(const Vector3 &w)
{
    return std::sqrt(Sin2Theta(w));
}

inline Scalar AbsCosTheta(const Vector3 &w)
{
    return std::abs(w.z());
}

inline Scalar SaturatedCosTheta(const Vector3 &w)
{
    return std::max(std::abs(w.z()), 0.f);
}

inline Scalar Saturate(Scalar v)
{
    return std::max(v, 0.f);
}

inline Scalar SaturateDot(const Vector3 &a, const Vector3 &b)
{
    return Saturate(a.dot(b));
}

template <typename T> bool IsNormalized(const T &v)
{
    return NearlyEqual(v.squaredNorm(), 1.0f);
}

template <typename T> bool IsNearlyZero(const T &v)
{
    return v.squaredNorm() < Eps;
}

inline bool IsNearlyZero(const Scalar &v)
{
    return v < Eps;
}

template <typename T> T Clamp(T value, T min, T max)
{
    return std::min(std::max(value, min), max);
}

template <typename T> T ClampLength(const T &v, Scalar max_length)
{
    Scalar length = v.norm();
    return v / length * std::min(length, max_length);
}

inline Vector3 Reflect(const Vector3 &w)
{
    return {-w.x(), -w.y(), w.z()};
}

inline Vector3 Reflect(const Vector3 &w_o, const Vector3 &w_m)
{
    return w_m * (w_o.dot(w_m) * 2.f) - w_o;
}

inline Vector3 Refract(const Vector3 &w_i, float eta_i_over_eta_t)
{
    auto cos_theta_i = CosTheta(w_i);
    auto sin_theta_i_2 = 1.f - cos_theta_i * cos_theta_i;
    auto sin_theta_t_2 = eta_i_over_eta_t * eta_i_over_eta_t * sin_theta_i_2;

    auto cos_theta_t = std::sqrt(1 - sin_theta_t_2) * (cos_theta_i > 0 ? -1.f : 1.f);

    return Vector3(-eta_i_over_eta_t * w_i.x(), -eta_i_over_eta_t * w_i.y(), cos_theta_t).normalized();
}

inline Vector3 GetPossibleMajorAxis(const Vector3 &normal)
{
    if (std::abs(normal.x()) < InvSqrt3)
    {
        return Right;
    }
    if (std::abs(normal.y()) < InvSqrt3)
    {
        return Front;
    }

    return Up;
}

inline void GetLocalAxisFromNormal(const Vector3 &normal, const Vector3 &major_axis, Vector3 &u, Vector3 &v, Vector3 &w)
{
    ASSERT(IsNormalized(normal));

    // Use majorAxis to create a coordinate system relative to world space
    u = normal.cross(major_axis).normalized();
    v = normal.cross(u);
    w = normal;
}

inline void GetLocalAxisFromNormal(const Vector3 &normal, Vector3 &u, Vector3 &v, Vector3 &w)
{
    ASSERT(IsNormalized(normal));

    // Find an axis that is not parallel to normal
    const Vector3 major_axis = GetPossibleMajorAxis(normal);

    // Use majorAxis to create a coordinate system relative to world space
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);
}

inline Vector3 TransformBasisToWorld(const Vector3 &dir, const Vector3 &u, const Vector3 &v, const Vector3 &w)
{
    return u * dir.x() + v * dir.y() + w * dir.z();
}

inline Vector3 TransformBasisToLocal(const Vector3 &dir, const Vector3 &u, const Vector3 &v, const Vector3 &w)
{
    auto x = dir.dot(u);
    auto y = dir.dot(v);
    auto z = dir.dot(w);

    return {x, y, z};
}

inline Vector3 TransformBasisToWorld(const Vector3 &dir, const Vector3 &normal, const Vector3 &major_axis)
{
    Vector3 u;
    Vector3 v;
    Vector3 w;
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);

    return TransformBasisToWorld(dir, u, v, w);
}

inline Vector3 TransformBasisToLocal(const Vector3 &dir, const Vector3 &normal, const Vector3 &major_axis)
{
    Vector3 u;
    Vector3 v;
    Vector3 w;
    GetLocalAxisFromNormal(normal, major_axis, u, v, w);

    return TransformBasisToLocal(dir, u, v, w);
}

inline Scalar SchlickApproximation(float cos_theta_i, Scalar r0)
{
    return Lerp(r0, 1.f, std::pow((1.f - cos_theta_i), 5.f));
}

inline Vector3 SchlickApproximation(float cos_theta_i, const Vector3 &r0)
{
    return Lerp(r0, Ones, std::pow((1.f - cos_theta_i), 5.f));
}

inline float FrDielectric(float cos_theta_i, float eta_i, float eta_t)
{
    // Potentially swap indices of refraction
    const bool entering = cos_theta_i > 0.f;
    if (!entering)
    {
        std::swap(eta_i, eta_t);
        cos_theta_i = -cos_theta_i;
    }

    // compute cosThetaT using Snell’s law>
    auto sin_theta_i = std::sqrt(1 - cos_theta_i * cos_theta_i);
    auto ref_idx = eta_i / eta_t;
    auto sin_theta_t = ref_idx * sin_theta_i;

    if (sin_theta_t >= 1.f)
    {
        return 1.f;
    }

    // auto cos_theta_t = std::sqrt(1 - sin_theta_t * sin_theta_t);

    // auto r_parl = ((eta_t * cos_theta_i) - (eta_i * cos_theta_t)) / ((eta_t * cos_theta_i) + (eta_i * cos_theta_t));
    // auto r_perp = ((eta_i * cos_theta_i) - (eta_t * cos_theta_t)) / ((eta_i * cos_theta_i) + (eta_t * cos_theta_t));

    // return (r_parl * r_parl + r_perp * r_perp) / 2.f;

    auto r0 = (1.f - ref_idx) / (1.f + ref_idx);
    r0 = r0 * r0;
    return SchlickApproximation(cos_theta_i, r0);
}

inline float DistributionGGX(const Vector3 &normal, const Vector3 &half, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float n_dot_h = SaturateDot(normal, half);
    float n_dot_h_2 = n_dot_h * n_dot_h;

    float num = a2;
    float denom = (n_dot_h_2 * (a2 - 1.0f) + 1.0f);
    denom = Pi * denom * denom;

    return num / denom;
}

inline Vector3 FrConductor(float cos_theta_i, const Vector3 &r0)
{
    ASSERT(cos_theta_i >= 0.f);

    return SchlickApproximation(cos_theta_i, r0);
}

template <typename T> inline Vector4 Vector2Vec4(const std::vector<T> &v, size_t vec_offset = 0)
{
    const auto *first_point = &v[vec_offset * 4];
    return Vector4{static_cast<Scalar>(first_point[0]), static_cast<Scalar>(first_point[1]),
                   static_cast<Scalar>(first_point[2]), static_cast<Scalar>(first_point[3])};
}

template <typename T> inline Vector3 Vector2Vec3(const std::vector<T> &v, size_t vec_offset = 0)
{
    const auto *first_point = &v[vec_offset * 3];
    return Vector3{static_cast<Scalar>(first_point[0]), static_cast<Scalar>(first_point[1]),
                   static_cast<Scalar>(first_point[2])};
}

template <typename T> inline Vector2 Vector2Vec2(const std::vector<T> &v, size_t vec_offset = 0)
{
    const auto *first_point = &v[vec_offset * 2];
    return Vector2{static_cast<Scalar>(first_point[0]), static_cast<Scalar>(first_point[1])};
}

template <typename T> inline T Rgba2Bgra(const T &value)
{
    return {value.z(), value.y(), value.x(), value.w()};
}

inline Color4 VecToColor(const Vector4 &value)
{
    return (value * MaxRGB).cast<uint8_t>();
}

inline Vector4 ColorToVec(const Color4 &color)
{
    return color.cast<Scalar>() / 255.f;
}

inline Vector3 CalculateNormal(const Vector3 &v0, const Vector3 &v1, const Vector3 &v2)
{
    auto v10 = v1 - v0;
    auto v20 = v2 - v0;

    auto normal = v20.cross(v10);

    return normal.normalized();
}

inline Rotation EulerRotationToRotationAxis(const Vector3 &rotation)
{
    return (Eigen::AngleAxis<Scalar>(rotation.x(), Right) * Eigen::AngleAxis<Scalar>(rotation.y(), Front) *
            Eigen::AngleAxis<Scalar>(rotation.z(), Up));
}

inline Rotation Vector4AsQuaternion(const Vector4 &vector)
{
    return Rotation(vector.w(), vector.x(), vector.y(), vector.z());
}

inline Vector3 TangentSpaceToWorldSpace(const Vector3 &tangent_normal, const Vector3 &tangent,
                                        const Vector3 &surface_normal, float headedness)
{
    auto bi_tangent = surface_normal.cross(tangent) * headedness;

    return TransformBasisToWorld(tangent_normal, tangent, bi_tangent, surface_normal).normalized();
}

inline Vector3 LinearToSrgb(const Vector3 &linear_color)
{
    return linear_color.array().pow(0.45f);
}

inline Vector3 SRGBtoLinear(const Vector3 &srgb)
{
    auto rgb = srgb.array();

    auto cutoff = (rgb < 0.04045f).cast<Scalar>();
    auto higher = ((rgb + 0.055f) / 1.055f).pow(2.4f);
    auto lower = rgb / 12.92f;

    return higher * (1.f - cutoff) + lower * cutoff;
}

inline Vector4 SRGBtoLinear(const Vector4 &srgba)
{
    Vector3 srgb = srgba.head<3>();

    return ConcatVector(SRGBtoLinear(srgb), srgba.w());
}

inline Scalar SmithGGXCorrelated(float cos_o, float cos_i, Scalar roughness)
{
    auto a = roughness * roughness;
    auto a2 = a * a;

    auto ggx_i = cos_o * std::sqrt((-cos_i * a2 + cos_i) * cos_i + a2);
    auto ggx_o = cos_i * std::sqrt((-cos_o * a2 + cos_o) * cos_o + a2);
    return 2.f * cos_o * cos_i / (ggx_o + ggx_i);
}

inline Scalar GeometrySchlickGGX(float cos_theta, float roughness)
{
    auto a = roughness * roughness;
    auto a2 = a * a;

    float tan2_v = (1.f - cos_theta * cos_theta) / (cos_theta * cos_theta);
    return 2.f / (1.f + std::sqrt(1.f + a2 * tan2_v));
}

inline Scalar GeometrySmith(float cos_o, float cos_i, float roughness)
{
    Scalar ggx1 = GeometrySchlickGGX(cos_o, roughness);
    Scalar ggx2 = GeometrySchlickGGX(cos_i, roughness);

    return ggx1 * ggx2;
}

inline Scalar SmithGGXMasking(const Vector3 &w_o, const Vector3 &normal, float roughness)
{
    auto a = roughness * roughness;
    auto a2 = a * a;

    float cos_o = SaturateDot(w_o, normal);

    float denom_c = std::sqrt(a2 + (1.0f - a2) * cos_o * cos_o) + cos_o;

    return 2.0f * cos_o / denom_c;
}

inline Mat4 ZUpToYUpMatrix()
{
    Mat4 axis_swizzle_matrix;

    axis_swizzle_matrix.setZero();
    axis_swizzle_matrix(0, 0) = 1.f;
    axis_swizzle_matrix(1, 2) = 1.f;
    axis_swizzle_matrix(2, 1) = -1.f;
    axis_swizzle_matrix(3, 3) = 1.f;

    return axis_swizzle_matrix;
}

// this function returns a unit vector
inline Vector3 SphericalToCartesian(Scalar cos_theta, Scalar sin_theta, Scalar cos_phi, Scalar sin_phi)
{
    return {cos_phi * sin_theta, sin_phi * sin_theta, cos_theta};
}

// this function returns a unit vector
inline Vector3 SphericalToCartesian(Scalar theta, Scalar phi)
{
    return SphericalToCartesian(std::cos(theta), std::sin(theta), std::cos(phi), std::sin(phi));
}

inline Vector2 EquirectangularToSpherical(const Vector2 &uv)
{
    // theta: [0, Pi]
    // phi: [-Pi, Pi]
    return {uv.y() * Pi, uv.x() * 2.f * Pi - Pi};
}

inline Vector2 SphericalToEquirectangular(const Vector2 &spherical)
{
    return {(spherical.x() + Pi) * 0.5f * InvPi, (spherical.y() + 0.5f * Pi) * InvPi};
}

inline Vector2 CartesianToEquirectangular(const Vector3 &v)
{
    static const Vector2 InvAtan(InvPi * 0.5f, InvPi);

    Vector2 uv(std::atan2(v.y(), v.x()), std::asin(-v.z()));
    uv = uv.cwiseProduct(InvAtan);
    uv += Vector2::Ones() * 0.5;
    return uv;
}

inline Vector3 EquirectangularToCartesian(const Vector2 &uv)
{
    auto sp = EquirectangularToSpherical(uv);
    return SphericalToCartesian(sp.x(), sp.y());
}

inline unsigned WrapMod(int a, unsigned b)
{
    return (static_cast<unsigned>(a % static_cast<int>(b)) + b) % b;
}

template <typename T> inline T AlignAddress(T address, unsigned alignment)
{
    return (address % alignment == 0) ? address : (address - address % alignment + alignment);
}

template <class T1, class T2> inline T1 DivideAndRoundUp(T1 a, T2 b)
{
    return (a + b - 1) / b;
}

template <class T> bool IsSame(T a, T b)
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return std::abs(a - b) < std::numeric_limits<T>::epsilon();
    }
    else
    {
        return a == b;
    }
}
}; // namespace utilities
} // namespace sparkle
