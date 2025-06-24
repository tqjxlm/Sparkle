#pragma once

#include "rhi/RHIResource.h"

#include "core/math/Types.h"

#include <unordered_set>

namespace sparkle
{
class RHIBuffer;

class RHIBLAS : public RHIResource
{
public:
    using RHIResource::RHIResource;

    RHIBLAS(const TransformMatrix &transform, const RHIResourceRef<RHIBuffer> &vertex_buffer,
            const RHIResourceRef<RHIBuffer> &index_buffer, uint32_t num_primitive, uint32_t num_vertex,
            const std::string &name)
        : RHIResource(name), transform_(std::move(transform)), vertex_buffer_(vertex_buffer),
          index_buffer_(index_buffer), num_primitive_(num_primitive), num_vertex_(num_vertex)
    {
    }

    [[nodiscard]] bool IsDirty() const
    {
        return is_dirty_;
    }

    [[nodiscard]] TransformMatrix GetTransform() const
    {
        return transform_;
    }

    void SetTransform(const TransformMatrix &new_transform)
    {
        transform_ = new_transform;
    }

protected:
    TransformMatrix transform_;
    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;
    uint32_t num_primitive_;
    uint32_t num_vertex_;

    bool is_dirty_ = true;
};

class RHITLAS : public RHIResource
{
public:
    using RHIResource::RHIResource;

    virtual void Build() = 0;

    virtual void Update(const std::unordered_set<uint32_t> &instances_to_update) = 0;

    void SetBLAS(RHIBLAS *blas, unsigned primitive_id)
    {
        if (all_blas_.size() < primitive_id + 1)
        {
            all_blas_.resize(primitive_id + 1);
        }

        if (all_blas_[primitive_id] == blas)
        {
            return;
        }

        all_blas_[primitive_id] = blas;

        // BLAS change will surely invalidate TLAS, so a new id should be generated
        id_dirty_ = true;
    }

    [[nodiscard]] const auto &GetBlasArray() const
    {
        return all_blas_;
    }

protected:
    // the array may not be contiguous. do validate when iterating through it.
    std::vector<RHIBLAS *> all_blas_;
};
} // namespace sparkle
