#include "renderer/proxy/MeshRenderProxy.h"

#include "core/Profiler.h"
#include "core/math/BVH.h"
#include "core/math/Intersection.h"
#include "core/math/Ray.h"
#include "core/math/Utilities.h"
#include "io/Mesh.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "rhi/RHI.h"
#include "rhi/RHIRayTracing.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#include <bvh/v2/default_builder.h>
#include <bvh/v2/executor.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/thread_pool.h>
#pragma GCC diagnostic pop

namespace sparkle
{
class BLAS
{
public:
    explicit BLAS(const Mesh *raw_mesh) : mesh_(raw_mesh)
    {
    }

    void Build()
    {
        using Vec3 = bvh::v2::Vec<Scalar, 3>;
        using BBox = bvh::v2::BBox<Scalar, 3>;
        bvh::v2::ThreadPool thread_pool;
        bvh::v2::ParallelExecutor executor(thread_pool);

        auto num_triangles = static_cast<uint32_t>(mesh_->indices.size()) / 3;
        triangles_.resize(num_triangles);

        std::vector<BBox> bboxes(num_triangles);
        std::vector<Vec3> centers(num_triangles);
        executor.for_each(0, num_triangles, [&](uint32_t begin, uint32_t end) {
            for (uint32_t i = begin; i < end; ++i)
            {
                Vector3 min;
                Vector3 max;
                mesh_->GetTriangleMinMax(i, min, max);

                bboxes[i] = BBox(ToBVHVec3(min), ToBVHVec3(max));
                centers[i] = ToBVHVec3((min + max) / 2);

                Vector3 v0;
                Vector3 v1;
                Vector3 v2;
                mesh_->GetTriangle(i, v0, v1, v2);

                triangles_[i].Set(v0, v1, v2);
            }
        });

        typename bvh::v2::DefaultBuilder<Node>::Config config;
        config.quality = bvh::v2::DefaultBuilder<Node>::Quality::High;
        bvh_ = bvh::v2::DefaultBuilder<Node>::build(thread_pool, bboxes, centers, config);
    }

    bool Intersect(const Ray &ray, const Transform &transform, IntersectionCandidate &candidate)
    {
        return IntersectInternal<false>(ray, transform, candidate);
    }

    bool IntersectAnyHit(const Ray &ray, const Transform &transform, IntersectionCandidate &candidate)
    {
        return IntersectInternal<true>(ray, transform, candidate);
    }

private:
    struct Triangle
    {
        void Set(const Vector3 &v0, const Vector3 &v1, const Vector3 &v2)
        {
            p0 = v0;
            e1 = v1 - v0;
            e2 = v2 - v0;
            n = e1.cross(e2);
        }

        Vector3 p0;
        Vector3 e1;
        Vector3 e2;
        Vector3 n;
    };

    template <bool AnyHit>
    bool IntersectInternal(const Ray &world_ray, const Transform &transform, IntersectionCandidate &candidate)
    {
        // all intersection tests are done in local space so we don't update mesh data when transform changes
        // the cost is higher intersection test cost
        auto inverse_transform = transform.GetInverse();
        auto local_ray = world_ray.TransformedBy(inverse_transform);

        static constexpr size_t InvalidId = std::numeric_limits<size_t>::max();
        static constexpr size_t StackSize = 32;
        static constexpr bool UseRobustTraversal = false;

        auto prim_id = InvalidId;

        bvh::v2::Ray<Scalar, 3> bvh_ray(ToBVHVec3(local_ray.Origin()), ToBVHVec3(local_ray.Direction()));

        bvh::v2::SmallStack<Bvh::Index, StackSize> stack;
        auto leaf_fn = [this, &world_ray, &local_ray, &prim_id, &candidate, &transform](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
            {
                if (IntersectTriangle<AnyHit>(world_ray, local_ray, transform, candidate,
                                              static_cast<uint32_t>(bvh_.prim_ids[i])))
                {
                    prim_id = i;
                }
            }
            return prim_id != InvalidId;
        };

        bvh_.intersect<AnyHit, UseRobustTraversal>(bvh_ray, bvh_.get_root().index, stack, leaf_fn);

        return prim_id != InvalidId;
    }

    template <bool AnyHit>
    bool IntersectTriangle(const Ray &world_ray, const Ray &local_ray, const Transform &transform,
                           IntersectionCandidate &candidate, uint32_t face_idx) const
    {
        // https://en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm

        const Vector3 direction = local_ray.Direction();
        const Vector3 origin = local_ray.Origin();

        const auto &triangle = triangles_[face_idx];

        const Vector3 e1 = triangle.e1;
        const Vector3 e2 = triangle.e2;
        const Vector3 p0 = triangle.p0;

        const Vector3 h = direction.cross(e2);

        auto a = e1.dot(h);
        if (std::abs(a) < Eps)
        {
            // This ray is parallel to this triangle
            return false;
        }

        const Vector3 s = origin - p0;
        auto f = 1.f / a;
        auto u = f * s.dot(h);
        if (u < 0.f || u > 1.f)
        {
            // outside of the triangle
            return false;
        }

        const Vector3 q = s.cross(e1);
        auto v = f * direction.dot(q);
        if (v < 0.f || u + v > 1.f)
        {
            // outside of the triangle
            return false;
        }

        auto t = f * e2.dot(q);
        if (t > Eps)
        {
            if constexpr (AnyHit)
            {
                // any way it must be a hit
                return true;
            }

            // check closest point in world space
            Vector3 world_p = transform.TransformPoint(local_ray.At(t));
            auto world_t = world_ray.InverseAt(world_p);

            if (world_t > 0 && candidate.IsCloserHit(world_t))
            {
                candidate.t = world_t;
                candidate.u = u;
                candidate.v = v;
                candidate.face_idx = face_idx;

                return true;
            }
        }

        return false;
    }

    const Mesh *mesh_;
    std::vector<Triangle> triangles_;
    Bvh bvh_;
};

MeshRenderProxy::MeshRenderProxy(const std::shared_ptr<const Mesh> &raw_mesh, std::string_view name,
                                 const AABB &local_bound)
    : PrimitiveRenderProxy(name, local_bound), raw_mesh_(raw_mesh)
{
    ASSERT(raw_mesh_);

    is_mesh_ = true;
}

MeshRenderProxy::~MeshRenderProxy() = default;

void MeshRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    PROFILE_SCOPE("MeshRenderProxy::InitRenderResources");

    PrimitiveRenderProxy::InitRenderResources(rhi, config);

    const RHIBuffer::BufferUsage ray_tracing_usages =
        config.IsRayTracingMode() ? RHIBuffer::BufferUsage::StorageBuffer : RHIBuffer::BufferUsage::None;

    const RHIBuffer::BufferUsage blas_usages =
        config.IsRayTracingMode()
            ? (RHIBuffer::BufferUsage::DeviceAddress | RHIBuffer::BufferUsage::AccelerationStructureBuildInput)
            : RHIBuffer::BufferUsage::None;

    vertex_buffer_ =
        rhi->CreateBuffer({.size = ARRAY_SIZE(raw_mesh_->vertices),
                           .usages = RHIBuffer::BufferUsage::VertexBuffer | ray_tracing_usages | blas_usages,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "MeshVertexBuffer");
    index_buffer_ =
        rhi->CreateBuffer({.size = ARRAY_SIZE(raw_mesh_->indices),
                           .usages = RHIBuffer::BufferUsage::IndexBuffer | ray_tracing_usages | blas_usages,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "MeshIndexBuffer");

    std::vector<VertexAttribute> attrib_buffer;
    attrib_buffer.reserve(raw_mesh_->GetNumVertices());

    for (unsigned i = 0; i < raw_mesh_->GetNumVertices(); i++)
    {
        attrib_buffer.emplace_back(VertexAttribute{
            .tangent = raw_mesh_->tangents[i], .normal = raw_mesh_->normals[i], .tex_coord = raw_mesh_->uvs[i]});
    }

    vertex_attrib_buffer_ =
        rhi->CreateBuffer({.size = ARRAY_SIZE(attrib_buffer),
                           .usages = RHIBuffer::BufferUsage::VertexBuffer | ray_tracing_usages,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "MeshVertexAttribBuffer");

    vertex_buffer_->UploadImmediate(raw_mesh_->vertices.data());
    index_buffer_->UploadImmediate(raw_mesh_->indices.data());
    vertex_attrib_buffer_->UploadImmediate(attrib_buffer.data());

    draw_args_.vertex_count = raw_mesh_->GetNumVertices();
    draw_args_.index_count = raw_mesh_->GetNumIndices();

    ubo_ = rhi->CreateBuffer({.size = sizeof(MeshUniform),
                              .usages = RHIBuffer::BufferUsage::UniformBuffer | RHIBuffer::BufferUsage::TransferDst,
                              .mem_properties = RHIMemoryProperty::DeviceLocal,
                              .is_dynamic = false},
                             "ModelUBO");

    if (config.IsRayTracingMode())
    {
        blas_ = rhi->CreateBLAS(GetTransform().GetMatrix(), GetVertexBuffer(), GetIndexBuffer(), GetNumFaces(),
                                GetNumVertices(), "MeshBLAS");
    }
}

void MeshRenderProxy::Render(RHIContext *rhi, const RHIResourceRef<RHIPipelineState> &pipeline_state) const
{
    rhi->DrawMesh(pipeline_state, draw_args_);
}

void MeshRenderProxy::UpdateMatrix(RHIContext *rhi)
{
    ASSERT(ubo_);

    MeshUniform ubo;

    auto matrix = GetTransform().GetMatrix();

    ubo.model_matrix = matrix;
    ubo.model_matrix_inv_transpose = ubo.model_matrix.inverse().transpose();
    ubo_->Upload(rhi, &ubo);

    if (blas_)
    {
        // TODO(tqjxlm): support movable blas
        blas_->SetTransform(matrix);
    }
}

uint32_t MeshRenderProxy::GetNumFaces() const
{
    return static_cast<uint32_t>(raw_mesh_->GetNumFaces());
}

uint32_t MeshRenderProxy::GetNumVertices() const
{
    return raw_mesh_->GetNumVertices();
}

void MeshRenderProxy::BuildBVH()
{
    accleration_structure_ = std::make_unique<BLAS>(raw_mesh_.get());

    accleration_structure_->Build();
}

template <bool AnyHit> bool MeshRenderProxy::IntersectInternal(const Ray &ray, IntersectionCandidate &candidate) const
{
    ASSERT(accleration_structure_);
    if constexpr (AnyHit)
    {
        return accleration_structure_->IntersectAnyHit(ray, GetTransform(), candidate);
    }
    else
    {
        return accleration_structure_->Intersect(ray, GetTransform(), candidate);
    }
}

bool MeshRenderProxy::IntersectAnyHit(const Ray &ray, IntersectionCandidate &candidate) const
{
    return IntersectInternal<true>(ray, candidate);
}

bool MeshRenderProxy::Intersect(const Ray &ray, IntersectionCandidate &candidate) const
{
    return IntersectInternal<false>(ray, candidate);
}

void MeshRenderProxy::GetIntersection(const Ray &ray, const IntersectionCandidate &candidate,
                                      Intersection &intersection)
{
    auto inv_transform = GetTransform().GetInverse();

    Vector2 tex_coord = raw_mesh_->GetTexCoord(candidate.face_idx, candidate.u, candidate.v);
    Vector3 surface_normal =
        raw_mesh_->GetShadingNormal(candidate.face_idx, candidate.u, candidate.v, candidate.geometry_normal);
    Vector4 tangent = raw_mesh_->GetTangent(candidate.face_idx, candidate.u, candidate.v);

    [[likely]] if (material_proxy_->HasNormalTexture())
    {
        auto tangent_normal = material_proxy_->GetNormal(tex_coord);

        surface_normal =
            utilities::TangentSpaceToWorldSpace(tangent_normal, tangent.head<3>(), surface_normal, tangent.w());
    }

    Vector3 world_normal = inv_transform.TransformDirectionTangentSpace(surface_normal).normalized();
    Vector3 world_tangent = inv_transform.TransformDirectionTangentSpace(tangent.head<3>());
    intersection.Update(ray, this, candidate.t, world_normal, world_tangent, tex_coord);
}

void MeshRenderProxy::OnTransformDirty(RHIContext *rhi)
{
    PrimitiveRenderProxy::OnTransformDirty(rhi);

    UpdateMatrix(rhi);
}
} // namespace sparkle
