#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
// lowers an RHIRenderingInfo into a new render pass descriptor
[[nodiscard]] MTLRenderPassDescriptor *CreateMetalRenderPassDescriptor(const RHIRenderingInfo &info);
} // namespace sparkle

#endif
