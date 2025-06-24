#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include "MetalShader.h"
#include "rhi/RHIResourceArray.h"

namespace sparkle
{
class MetalResourceArray : public RHIResourceArray
{
public:
    using RHIResourceArray::RHIResourceArray;

    void Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point);

private:
    void Setup();

    void UpdateResources();

    MetalShader::ArgumentBuffer argument_buffer_;
};
} // namespace sparkle

#endif
