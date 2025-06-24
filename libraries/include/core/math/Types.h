#pragma once

#include <Eigen/Dense>

#include <numbers>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

namespace sparkle
{
using Scalar = float;
using Half = Eigen::half;

using Color4 = Eigen::Vector4<uint8_t>;
using Vector2 = Eigen::Vector<Scalar, 2>;
using Vector3 = Eigen::Vector<Scalar, 3>;
using Vector4 = Eigen::Vector<Scalar, 4>;
using Vector2d = Eigen::Vector<double, 2>;
using Vector3d = Eigen::Vector<double, 3>;
using Vector4d = Eigen::Vector<double, 4>;
using Vector2h = Eigen::Vector<Half, 2>;
using Vector3h = Eigen::Vector<Half, 3>;
using Vector4h = Eigen::Vector<Half, 4>;
using Vector2UInt = Eigen::Vector<uint32_t, 2>;
using Vector3UInt = Eigen::Vector<uint32_t, 3>;
using Vector4UInt = Eigen::Vector<uint32_t, 4>;
using Vector2Int = Eigen::Vector<int32_t, 2>;
using Vector3Int = Eigen::Vector<int32_t, 3>;
using Vector4Int = Eigen::Vector<int32_t, 4>;
using Quaternion = Eigen::Quaternion<Scalar>;
using Rotation = Eigen::AngleAxis<Scalar>::QuaternionType;
using TransformData = Eigen::Transform<Scalar, 3, Eigen::Affine>;
using TransformMatrix = TransformData::MatrixType;
using Mat2x2 = Eigen::Matrix<Scalar, 2, 2>;
using Mat3x3 = Eigen::Matrix<Scalar, 3, 3>;
using Mat4x4 = Eigen::Matrix<Scalar, 4, 4>;
using Mat4x3 = Eigen::Matrix<Scalar, 4, 3>;
using Mat4x2 = Eigen::Matrix<Scalar, 4, 2>;
using Mat3x4 = Eigen::Matrix<Scalar, 3, 4>;
using Mat2 = Mat2x2;
using Mat3 = Mat3x3;
using Mat4 = Mat4x4;

static const Vector3 Up = Vector3::UnitZ();
static const Vector3 Front = Vector3::UnitY();
static const Vector3 Right = Vector3::UnitX();
static const Vector3 Ones = Vector3::Ones();
static const Vector3 Zeros = Vector3::Zero();

constexpr Scalar Infinity = std::numeric_limits<Scalar>::infinity();
constexpr Scalar Pi = std::numbers::pi_v<float>;
constexpr Scalar InvPi = std::numbers::inv_pi_v<float>;
constexpr Scalar Eps = 1e-6f;
constexpr Scalar Tolerance = 1e-4f;
constexpr Scalar InvSqrt3 = std::numbers::inv_sqrt3_v<float>;
constexpr Scalar MaxRGB = 255.f;
} // namespace sparkle

#pragma GCC diagnostic pop
