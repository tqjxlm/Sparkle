#pragma once

#define RANDOM_USE_XOSHIRO 1

#include "core/math/Utilities.h"

#include <thread>
#if RANDOM_USE_XOSHIRO
#include <XoshiroCpp.hpp>
#else
#include <random>
#endif

namespace sparkle
{
namespace sampler
{
template <bool FixSeed = false> inline float RandomUnit()
{
    constexpr unsigned int FixedSeed = 42;
    static thread_local unsigned int seed =
        FixSeed ? FixedSeed : static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

#if RANDOM_USE_XOSHIRO
    static thread_local XoshiroCpp::Xoshiro128Plus rng(seed);
    return static_cast<float>(rng()) / static_cast<float>(std::numeric_limits<uint32_t>::max());
#else
    static thread_local std::mt19937 generator(seed);

    std::uniform_real_distribution<float> distribution(0.f, 1.f);
    return distribution(generator);
#endif
}

template <bool FixSeed = false> inline Vector2 UnitDisk()
{
    auto rand = RandomUnit<FixSeed>();
    auto theta = RandomUnit<FixSeed>() * 2.0f * Pi;

    auto r = std::sqrt(rand);
    auto x = r * std::cos(theta);
    auto y = r * std::sin(theta);

    return {x, y};
}

struct UniformHemiSphere
{
    static Vector3 Sample()
    {
        // https://alexanderameye.github.io/notes/sampling-the-hemisphere/

        auto cos_theta = RandomUnit();
        auto epsilon = RandomUnit();

        auto sin_theta = sqrt(1 - cos_theta * cos_theta);

        auto phi = 2 * Pi * epsilon;
        auto cos_phi = cos(phi);
        auto sin_phi = sin(phi);

        return utilities::SphericalToCartesian(cos_theta, sin_theta, cos_phi, sin_phi);
    }

    static Scalar Pdf()
    {
        return InvPi * 0.5f;
    }
};

struct CosineWeightedHemiSphere
{
    static Vector3 Sample()
    {
        Vector2 unit_disk = UnitDisk();

        // Project z up to the unit hemisphere
        auto z = std::sqrt(std::max(0.f, 1.0f - unit_disk.squaredNorm()));

        return {unit_disk.x(), unit_disk.y(), z};
    }

    static Scalar Pdf(const Vector3 &w_m)
    {
        return utilities::AbsCosTheta(w_m) * InvPi;
    }
};

struct DistributionGGX
{
    static Vector3 Sample(float roughness)
    {
        auto u_1 = RandomUnit();
        auto u_2 = RandomUnit();

        auto a = roughness * roughness;
        auto a2 = a * a;
        auto cos_theta_2 = (1.f - u_1) / ((a2 - 1.f) * u_1 + 1.f);
        auto cos_theta = std::sqrt(cos_theta_2);
        auto theta = std::acos(cos_theta);

        auto phi = 2.f * Pi * u_2;

        return utilities::SphericalToCartesian(theta, phi);
    }

    static Scalar Ndf(float cos_theta, float roughness)
    {
        auto a = roughness * roughness;
        auto a2 = a * a;

        auto d = (a2 - 1.f) * cos_theta * cos_theta + 1.f;

        return a2 / (Pi * d * d);
    }

    static Scalar Pdf(const Vector3 &w_m, float roughness)
    {
        auto cos_theta = utilities::CosTheta(w_m);

        return cos_theta * Ndf(cos_theta, roughness);
    }
};

struct DistributionVn
{
    static Vector3 Sample(const Vector3 &w_o, float roughness)
    {
        float a = roughness * roughness;

        auto u1 = RandomUnit();
        auto u2 = RandomUnit();

        // -- Stretch the view vector so we are sampling as though
        // -- roughness==1
        Vector3 v = Vector3(w_o.x() * a, w_o.y() * a, w_o.z()).normalized();

        // -- Build an orthonormal basis with v, t1, and t2
        Vector3 t1 = (v.z() < 0.999f) ? v.cross(Up).normalized() : Right;
        Vector3 t2 = v.cross(t1);

        // -- Choose a point on a disk with each half of the disk weighted
        // -- proportionally to its projection onto direction v
        auto r = std::sqrt(u1);
        auto phi = 2.f * Pi * u2;
        auto p1 = r * std::cos(phi);
        auto p2 = r * std::sin(phi);
        auto s = 0.5f * (1.f + v.z());
        p2 = (1.f - s) * std::sqrt(1.f - p1 * p1) + s * p2;

        // -- Calculate the normal in this stretched tangent space
        auto n = p1 * t1 + p2 * t2 + std::sqrt(std::max(0.f, 1.f - p1 * p1 - p2 * p2)) * v;

        // -- unstretch and normalize the normal
        return Vector3(a * n.x(), a * n.y(), std::max(0.f, n.z())).normalized();
    }
};

// sample a mircro-facet normal given a surface normal
inline Vector3 SampleMicroFacetNormal(float roughness)
{
    return DistributionGGX::Sample(roughness);
}

inline Vector3 SampleMicroFacetNormal(const Vector3 &w_o, float roughness)
{
    return DistributionVn::Sample(w_o, roughness);
}
}; // namespace sampler
} // namespace sparkle
