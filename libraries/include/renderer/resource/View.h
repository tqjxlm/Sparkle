#pragma once

#include "core/math/Types.h"

namespace sparkle
{
struct ViewUBO
{
#if PLATFORM_WINDOWS
// minwindef.h defines these names...
#undef near
#undef far
#endif
    Mat4 view_projection_matrix;
    Mat4 view_matrix;
    Mat4 projection_matrix;
    Mat4 inv_view_matrix;
    Mat4 inv_projection_matrix;
    float near;
    float far;
};
} // namespace sparkle
