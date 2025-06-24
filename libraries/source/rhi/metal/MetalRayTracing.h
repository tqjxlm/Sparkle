#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
class MetalBLAS : public RHIBLAS
{
public:
    using RHIBLAS::RHIBLAS;

    void Build(id<MTLAccelerationStructureCommandEncoder> command_encoder);

    [[nodiscard]] id<MTLAccelerationStructure> GetAccelerationStructure() const
    {
        return acceleration_structure_;
    }

private:
    id<MTLAccelerationStructure> acceleration_structure_;
};

class MetalTLAS : public RHITLAS
{
public:
    using RHITLAS::RHITLAS;

    [[nodiscard]] id<MTLAccelerationStructure> GetResource() const
    {
        return tlas_;
    }

    void Build() override;

    void Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const;

    void Update(const std::unordered_set<uint32_t> &instances_to_update) override;

private:
    RHIResourceRef<RHIBuffer> blas_descriptor_buffer_;
    id<MTLAccelerationStructure> tlas_;
    NSMutableArray *blas_array_;
};
} // namespace sparkle

#endif
