#pragma once

#include "core/math/Types.h"
#include "rhi/RHIResource.h"

#include <array>
#include <vector>

namespace sparkle
{
class RHIBuffer;
class RHIContext;
class SceneRenderProxy;

// the scene's emissive triangles in world space with an alias table over their emitted power, which the gpu path
// tracer samples for next event estimation. the buffers always hold an element, so they bind before any emitter exists
class EmissiveTriangles
{
public:
    // must match EmissiveTriangle in shaders/ray_trace/ray_trace.cs.slang
    struct alignas(16) Triangle
    {
        std::array<Vector4, 3> vertices;
        // the probability of sampling this triangle over its area
        float pdf_area;
        uint32_t instance_id;
        uint32_t triangle_id;
    };

    explicit EmissiveTriangles(RHIContext *rhi);

    ~EmissiveTriangles();

    // whether this frame's scene changes can change the emitters
    [[nodiscard]] bool NeedsRebuild(const SceneRenderProxy &scene) const;

    void Rebuild(RHIContext *rhi, const SceneRenderProxy &scene);

    // the number of triangles the sampling table can pick
    [[nodiscard]] uint32_t GetCount() const
    {
        return count_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetTriangles() const
    {
        return triangles_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetSamplingTable() const
    {
        return sampling_table_;
    }

    // each primitive's first triangle, UINT_MAX if it emits nothing
    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetPrimitiveOffsets() const
    {
        return primitive_offsets_;
    }

private:
    [[nodiscard]] bool IsEmissive(uint32_t primitive_index) const;

    RHIResourceRef<RHIBuffer> triangles_;
    RHIResourceRef<RHIBuffer> sampling_table_;
    RHIResourceRef<RHIBuffer> primitive_offsets_;

    std::vector<uint32_t> offsets_;

    uint32_t count_ = 0;
};
} // namespace sparkle
