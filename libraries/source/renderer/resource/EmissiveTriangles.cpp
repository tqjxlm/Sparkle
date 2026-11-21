#include "renderer/resource/EmissiveTriangles.h"

#include "core/math/AliasTable.h"
#include "io/Mesh.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

#include <unordered_map>

namespace sparkle
{
static_assert(sizeof(EmissiveTriangles::Triangle) == 64);

// binds as a storage buffer even when data is empty
template <class T>
static RHIResourceRef<RHIBuffer> CreateStorageBuffer(RHIContext *rhi, std::vector<T> data, const std::string &name)
{
    data.resize(std::max<size_t>(data.size(), 1));

    auto buffer = rhi->CreateBuffer({.size = sizeof(T) * data.size(),
                                     .usages = RHIBuffer::BufferUsage::StorageBuffer,
                                     .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                     .is_dynamic = false},
                                    name);
    buffer->UploadImmediate(data.data());
    return buffer;
}

// the luminance a material emits, averaged over its texture
static float AverageEmittedLuminance(const MaterialRenderProxy &material)
{
    constexpr unsigned Grid = 16;

    Vector3 sum = Zeros;
    for (unsigned y = 0; y < Grid; y++)
    {
        for (unsigned x = 0; x < Grid; x++)
        {
            sum += material.GetEmissive(Vector2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) / Grid);
        }
    }

    return utilities::Luminance(sum) / (Grid * Grid);
}

EmissiveTriangles::EmissiveTriangles(RHIContext *rhi)
    : triangles_(CreateStorageBuffer<Triangle>(rhi, {}, "EmissiveTriangles")),
      sampling_table_(CreateStorageBuffer<AliasTableEntry>(rhi, {}, "EmissiveSamplingTable")),
      primitive_offsets_(CreateStorageBuffer<uint32_t>(rhi, {}, "EmissivePrimitiveOffsets"))
{
}

EmissiveTriangles::~EmissiveTriangles() = default;

bool EmissiveTriangles::IsEmissive(uint32_t primitive_index) const
{
    return primitive_index < offsets_.size() && offsets_[primitive_index] != UINT_MAX;
}

bool EmissiveTriangles::NeedsRebuild(const SceneRenderProxy &scene) const
{
    if (!scene.GetNewMaterialProxiesThisFrame().empty())
    {
        return true;
    }

    // a transform update moves only the triangles of its own primitive
    return std::ranges::any_of(scene.GetPrimitiveChangeList(), [this](const SceneRenderProxy::PrimitiveChange &change) {
        return change.type != SceneRenderProxy::PrimitiveChangeType::Update || IsEmissive(change.to_id);
    });
}

void EmissiveTriangles::Rebuild(RHIContext *rhi, const SceneRenderProxy &scene)
{
    const auto &primitives = scene.GetPrimitives();
    offsets_.assign(primitives.size(), UINT_MAX);

    std::vector<Triangle> triangles;
    std::vector<float> powers;
    std::unordered_map<const MaterialRenderProxy *, float> luminance_cache;

    for (auto *primitive : primitives)
    {
        if (!primitive->IsMesh())
        {
            continue;
        }

        const auto *material = primitive->GetMaterialRenderProxy();
        auto [cached, inserted] = luminance_cache.try_emplace(material, 0.f);
        if (inserted)
        {
            cached->second = AverageEmittedLuminance(*material);
        }

        const float luminance = cached->second;
        if (luminance <= 0.f)
        {
            continue;
        }

        const auto &mesh = primitive->As<MeshRenderProxy>()->GetMesh();
        const auto transform = primitive->GetTransform();
        const auto primitive_index = primitive->GetPrimitiveIndex();
        offsets_[primitive_index] = static_cast<uint32_t>(triangles.size());

        for (uint32_t face = 0; face < mesh.GetNumFaces(); face++)
        {
            Triangle triangle{.vertices = {}, .pdf_area = 0.f, .instance_id = primitive_index, .triangle_id = face};
            std::array<Vector3, 3> positions;
            for (uint32_t corner = 0; corner < 3; corner++)
            {
                positions[corner] = transform.TransformPoint(mesh.vertices[mesh.indices[face * 3 + corner]]);
                triangle.vertices[corner] = utilities::ConcatVector(positions[corner], 1.f);
            }

            // the area until the sampling table turns it into the density
            triangle.pdf_area = 0.5f * (positions[1] - positions[0]).cross(positions[2] - positions[0]).norm();
            powers.push_back(triangle.pdf_area * luminance);
            triangles.push_back(triangle);
        }
    }

    const auto table = BuildAliasTable(powers);
    for (size_t i = 0; i < table.size(); i++)
    {
        triangles[i].pdf_area = triangles[i].pdf_area > 0.f ? table[i].probability / triangles[i].pdf_area : 0.f;
    }
    count_ = static_cast<uint32_t>(table.size());

    // in-flight frames keep the previous buffers alive through deferred deletion
    triangles_ = CreateStorageBuffer(rhi, std::move(triangles), "EmissiveTriangles");
    sampling_table_ = CreateStorageBuffer(rhi, table, "EmissiveSamplingTable");
    primitive_offsets_ = CreateStorageBuffer(rhi, offsets_, "EmissivePrimitiveOffsets");
}
} // namespace sparkle
