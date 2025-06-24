#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{

class MetalShader : public RHIShader
{
public:
    struct ArgumentBuffer
    {
        id<MTLBuffer> buffer = nullptr;
        std::vector<id<MTLResource>> resources;
    };

    explicit MetalShader(const RHIShaderInfo *shader_info) : RHIShader(shader_info)
    {
    }

    void Load() override;

    [[nodiscard]] id<MTLFunction> GetFunction() const
    {
        ASSERT(IsValid());
        return function_;
    }

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    void SetupArgumentBuffers(id<MTLDevice> device, std::vector<ArgumentBuffer> &buffers,
                              RHIShaderResourceTable *resource_table) const;
#endif

private:
    id<MTLFunction> function_;
    id<MTLLibrary> library_;
};
} // namespace sparkle

#endif
