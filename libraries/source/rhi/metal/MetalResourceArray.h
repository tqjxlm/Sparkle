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

protected:
    void OnResourceUpdate() override;

private:
    struct FrameCopy
    {
        MetalShader::ArgumentBuffer argument_buffer;
        std::vector<unsigned> dirty_indices;
    };

    void Setup();

    void UpdateResources(FrameCopy &copy);

    // one argument buffer per frame in flight, so patching never touches a buffer the GPU may still read
    std::vector<FrameCopy> frame_copies_;
};
} // namespace sparkle

#endif
