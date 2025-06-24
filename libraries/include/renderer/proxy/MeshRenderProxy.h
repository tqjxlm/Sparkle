#pragma once

#include "renderer/proxy/PrimitiveRenderProxy.h"

#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
struct Mesh;
class BLAS;

class MeshRenderProxy : public PrimitiveRenderProxy
{
public:
    struct alignas(16) VertexAttribute
    {
        Vector4 tangent;
        alignas(16) Vector3 normal;
        alignas(8) Vector2 tex_coord;
    };

    struct MeshUniform
    {
        Mat4 model_matrix;
        Mat4 model_matrix_inv_transpose;
    };

    explicit MeshRenderProxy(const std::shared_ptr<const Mesh> &raw_mesh, std::string_view name,
                             const AABB &local_bound);

    ~MeshRenderProxy() override;

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config) override;

    void OnTransformDirty(RHIContext *rhi) override;

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetVertexBuffer() const
    {
        return vertex_buffer_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetIndexBuffer() const
    {
        return index_buffer_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetVertexAttribBuffer() const
    {
        return vertex_attrib_buffer_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetUniformBuffer() const
    {
        return ubo_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBLAS> &GetAccelerationStructure() const
    {
        return blas_;
    }

    [[nodiscard]] uint32_t GetNumFaces() const;

    [[nodiscard]] uint32_t GetNumVertices() const;

    void Render(RHIContext *rhi, const RHIResourceRef<RHIPipelineState> &pipeline_state) const;

    bool Intersect(const Ray &ray, IntersectionCandidate &candidate) const override;

    bool IntersectAnyHit(const Ray &ray, IntersectionCandidate &candidate) const override;

    void GetIntersection(const Ray &ray, const IntersectionCandidate &candidate, Intersection &intersection) override;

    template <bool AnyHit> bool IntersectInternal(const Ray &ray, IntersectionCandidate &candidate) const;

    template <bool AnyHit>
    bool IntersectTriangle(const Ray &ray, const Ray &local_ray, const Transform &inv_transform,
                           IntersectionCandidate &candidate, uint32_t face_idx) const;

    void BuildBVH() override;

protected:
    void UpdateMatrix(RHIContext *rhi);

private:
    std::shared_ptr<const Mesh> raw_mesh_;

    DrawArgs draw_args_;

    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;
    RHIResourceRef<RHIBuffer> vertex_attrib_buffer_;
    RHIResourceRef<RHIBuffer> ubo_;

    RHIResourceRef<RHIBLAS> blas_;

    std::unique_ptr<BLAS> accleration_structure_;
};
} // namespace sparkle
