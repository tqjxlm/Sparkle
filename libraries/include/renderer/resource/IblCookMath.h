#pragma once

#include "core/math/Types.h"
#include "io/Image.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// verbatim ports of shaders/include/{math,random,sampler,cubemap}.h.slang used by the
// IBL cook shaders. the CPU cook jobs must produce the same artifacts as the GPU passes
// (enforced by the ibl_parity test case), so any change on either side must be mirrored
// and the affected artifact versions bumped.
namespace sparkle::ibl_cook
{
constexpr float ShaderPi = std::numbers::pi_v<float>;
constexpr float ShaderEps = 1e-6f;
constexpr float ShaderInvSqrt3 = std::numbers::inv_sqrt3_v<float>;

struct ShaderRandom
{
    uint32_t state;

    ShaderRandom(uint32_t x, uint32_t y, uint32_t seed)
    {
        seed += x * 1664525u + y * 214013u;
        seed ^= seed << 13u;
        seed ^= seed >> 17u;
        seed ^= seed << 5u;
        state = seed;
    }

    uint32_t NextUint()
    {
        uint32_t prev = state;
        state = state * 747796405u + 2891336453u;
        uint32_t word = ((prev >> ((prev >> 28u) + 4u)) ^ prev) * 277803737u;
        return (word >> 22u) ^ word;
    }

    float RandomUnit()
    {
        return static_cast<float>(NextUint()) / 4294967296.0f;
    }

    Eigen::Vector2f UnitDisk()
    {
        float rand = RandomUnit();
        float theta = RandomUnit() * 2.0f * ShaderPi;

        float r = std::sqrt(rand);
        return {r * std::cos(theta), r * std::sin(theta)};
    }
};

inline float SaturateDot(const Vector3 &a, const Vector3 &b)
{
    return std::clamp(a.dot(b), 0.f, 1.f);
}

inline Vector3 Reflect(const Vector3 &i, const Vector3 &n)
{
    return i - 2.f * i.dot(n) * n;
}

inline Vector3 ClampToLength(const Vector3 &v, float max_length)
{
    const float length_v = v.norm();
    return (length_v > max_length) ? Vector3(v * (max_length / length_v)) : v;
}

inline float GeometrySchlickGGX(float cos_theta, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float tan2_v = (1.f - cos_theta * cos_theta) / (cos_theta * cos_theta + ShaderEps);
    return 2.f / (1.f + std::sqrt(1.f + a2 * tan2_v));
}

inline float SmithGGX(float cos_o, float cos_i, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float ggx_i = cos_o * std::sqrt((-cos_i * a2 + cos_i) * cos_i + a2 + ShaderEps);
    float ggx_o = cos_i * std::sqrt((-cos_o * a2 + cos_o) * cos_o + a2 + ShaderEps);
    return 2.f * cos_o * cos_i / (ggx_o + ggx_i + ShaderEps);
}

inline Vector3 SampleMicroFacetNormal(ShaderRandom &rng, const Vector3 &w_o, float roughness)
{
    float a = roughness * roughness;

    if (a < ShaderEps)
    {
        return {0.f, 0.f, 1.f};
    }

    float u1 = rng.RandomUnit();
    float u2 = rng.RandomUnit();

    Vector3 v = Vector3(w_o.x() * a, w_o.y() * a, w_o.z()).normalized();

    Vector3 t1 = (v.z() < 0.999f) ? Vector3(v.cross(Vector3(0.f, 0.f, 1.f))).normalized() : Vector3(1.f, 0.f, 0.f);
    Vector3 t2 = v.cross(t1);

    float r = std::sqrt(u1);
    float phi = 2.f * ShaderPi * u2;
    float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);
    float s = 0.5f * (1.f + v.z());
    p2 = (1.f - s) * std::sqrt(1.f - p1 * p1) + s * p2;

    Vector3 n = p1 * t1 + p2 * t2 + std::sqrt(std::max(0.f, 1.f - p1 * p1 - p2 * p2)) * v;

    return Vector3(a * n.x(), a * n.y(), std::max(0.f, n.z())).normalized();
}

inline Vector3 CosineWeightedHemisphereSample(ShaderRandom &rng)
{
    Eigen::Vector2f unit_disk = rng.UnitDisk();

    float z = std::sqrt(std::max(0.0f, 1.0f - unit_disk.dot(unit_disk)));

    return Vector3(unit_disk.x(), unit_disk.y(), z).normalized();
}

inline Vector3 GetPossibleMajorAxis(const Vector3 &normal)
{
    if (std::abs(normal.x()) < ShaderInvSqrt3)
    {
        return {1.f, 0.f, 0.f};
    }
    if (std::abs(normal.y()) < ShaderInvSqrt3)
    {
        return {0.f, 1.f, 0.f};
    }

    return {0.f, 0.f, 1.f};
}

inline void GetLocalAxisFromNormal(const Vector3 &normal, Vector3 &u, Vector3 &v, Vector3 &w)
{
    const Vector3 major_axis = GetPossibleMajorAxis(normal);

    u = normal.cross(major_axis).normalized();
    v = normal.cross(u);
    w = normal;
}

inline Vector3 TransformBasisToWorld(const Vector3 &dir, const Vector3 &u, const Vector3 &v, const Vector3 &w)
{
    return u * dir.x() + v * dir.y() + w * dir.z();
}

inline Vector3 GetDirectionFromCubeMapUV(float u_norm, float v_norm, uint32_t face_id)
{
    float u = u_norm * 2.f - 1.f;
    float v = v_norm * 2.f - 1.f;

    Vector3 direction;
    switch (face_id)
    {
    case 0:
        direction = {1.f, -v, -u};
        break;
    case 1:
        direction = {-1.f, -v, u};
        break;
    case 2:
        direction = {u, 1.f, v};
        break;
    case 3:
        direction = {u, -1.f, -v};
        break;
    case 4:
        direction = {u, -v, 1.f};
        break;
    case 5:
        direction = {-u, -v, -1.f};
        break;
    default:
        return {0.f, 0.f, 0.f};
    }

    return direction.normalized();
}

inline Vector4 CubeTexelSeamless(const Image2DCube &cube, uint32_t face_id, int x, int y)
{
    const Image2D &face = cube.GetFace(static_cast<Image2DCube::FaceId>(face_id));
    const int width = static_cast<int>(face.GetWidth());
    const int height = static_cast<int>(face.GetHeight());

    if (x >= 0 && x < width && y >= 0 && y < height)
    {
        return face.AccessPixel(static_cast<unsigned>(x), static_cast<unsigned>(y));
    }

    // out-of-face tap: resolve it geometrically through the tap's direction, which is what
    // seamless hardware cube filtering effectively does at face edges
    const Vector3 tap_direction =
        GetDirectionFromCubeMapUV((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(height), face_id);

    Vector2 uv;
    Image2DCube::FaceId adjacent_face_id;
    Image2DCube::DirectionToTextureCoordinate(tap_direction, uv, adjacent_face_id);

    const Image2D &adjacent_face = cube.GetFace(adjacent_face_id);
    const int adjacent_width = static_cast<int>(adjacent_face.GetWidth());
    const int adjacent_height = static_cast<int>(adjacent_face.GetHeight());

    const auto adjacent_x = std::clamp(
        static_cast<int>(std::lround(uv.x() * static_cast<float>(adjacent_width) - 0.5f)), 0, adjacent_width - 1);
    const auto adjacent_y = std::clamp(
        static_cast<int>(std::lround(uv.y() * static_cast<float>(adjacent_height) - 0.5f)), 0, adjacent_height - 1);

    return adjacent_face.AccessPixel(static_cast<unsigned>(adjacent_x), static_cast<unsigned>(adjacent_y));
}

// hardware texel-center convention (uv * size - 0.5) with seamless edge taps, unlike
// Image2D::Sample's (size - 1) face-local mapping: the specular mirror mip is a direct
// env lookup, so both differences show up at high-contrast and border texels
inline Vector3 SampleCubeHardware(const Image2DCube &cube, const Vector3 &direction)
{
    Vector2 uv;
    Image2DCube::FaceId face_id;
    Image2DCube::DirectionToTextureCoordinate(direction, uv, face_id);

    const Image2D &face = cube.GetFace(face_id);
    const int width = static_cast<int>(face.GetWidth());
    const int height = static_cast<int>(face.GetHeight());

    const float x = uv.x() * static_cast<float>(width) - 0.5f;
    const float y = uv.y() * static_cast<float>(height) - 0.5f;

    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    const Vector4 lerped =
        (CubeTexelSeamless(cube, face_id, x0, y0) * (1.f - fx) + CubeTexelSeamless(cube, face_id, x0 + 1, y0) * fx) *
            (1.f - fy) +
        (CubeTexelSeamless(cube, face_id, x0, y0 + 1) * (1.f - fx) +
         CubeTexelSeamless(cube, face_id, x0 + 1, y0 + 1) * fx) *
            fy;

    return lerped.head<3>();
}
} // namespace sparkle::ibl_cook
