#pragma once

#include "core/math/Types.h"

#if PLATFORM_WINDOWS
// minwindef.h defines these names...
#undef near
#undef far
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#include <bvh/v2/bvh.h>
#include <bvh/v2/node.h>
#pragma GCC diagnostic pop

namespace sparkle
{
using Node = bvh::v2::Node<Scalar, 3>;
using Bvh = bvh::v2::Bvh<Node>;

inline bvh::v2::Vec<Scalar, 3> ToBVHVec3(const Vector3 &v)
{
    return {v.x(), v.y(), v.z()};
}
} // namespace sparkle
