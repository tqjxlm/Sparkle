#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include <string>
#include <unordered_set>

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

    MetalShader(const RHIShaderInfo *shader_info, std::string variant) : RHIShader(shader_info, std::move(variant))
    {
    }

    void Load() override;

    [[nodiscard]] id<MTLFunction> GetFunction() const
    {
        ASSERT(IsValid());
        return function_;
    }

    // whether the entry point reads the resource `name` from a color attachment ([[color(n)]]), which has no binding
    [[nodiscard]] bool FetchesFramebuffer(const std::string &name) const
    {
        return framebuffer_fetches_.contains(name);
    }

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    void SetupArgumentBuffers(id<MTLDevice> device, std::vector<ArgumentBuffer> &buffers,
                              RHIShaderResourceTable *resource_table) const;
#endif

private:
    id<MTLFunction> function_;
    id<MTLLibrary> library_;
    std::unordered_set<std::string> framebuffer_fetches_;
};
} // namespace sparkle

#endif
