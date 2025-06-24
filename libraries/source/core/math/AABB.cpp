#include "core/math/AABB.h"

#include <format>

namespace sparkle
{
std::string AABB::ToString() const
{
    return std::format("{0}-{1}", utilities::VectorToString(center_), utilities::VectorToString(half_size_));
}
} // namespace sparkle
