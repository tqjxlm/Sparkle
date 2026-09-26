#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalBuffer : public RHIBuffer
{
public:
    MetalBuffer(const RHIBuffer::Attribute &attribute, const std::string &name);

    void CopyToBuffer(id<MTLBlitCommandEncoder> encoder, const RHIBuffer *buffer) const;

    void CopyToImage(id<MTLBlitCommandEncoder> encoder, const RHIImage *image) const;

    void *Lock() override
    {
        return buffer_.contents;
    }

    void UnLock() override
    {
#if FRAMEWORK_MACOS
        if (storage_option_ == MTLStorageModeManaged)
        {
            [buffer_ didModifyRange:{0, GetSize()}];
        }
        else
#endif
        {
            // shared storage on unified memory needs no explicit synchronization
            ASSERT(storage_option_ == MTLStorageModeShared);
        }
    }

    [[nodiscard]] id<MTLBuffer> GetResource() const;

    void Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const;

private:
    id<MTLBuffer> buffer_;
    MTLStorageMode storage_option_;
};

inline MTLStorageMode GetMetalStorageMode(RHIMemoryProperty memory_property)
{
    if (memory_property & RHIMemoryProperty::DeviceLocal)
    {
        return MTLStorageModePrivate;
    }

    // on apple silicon, it is always shared memory unless specifically specified.
    return MTLStorageModeShared;
}

inline MTLResourceOptions GetManagedBufferStorageMode()
{
#if !FRAMEWORK_IOS
    return MTLResourceStorageModeManaged;
#else
    return MTLResourceStorageModeShared;
#endif
}
} // namespace sparkle

#endif
