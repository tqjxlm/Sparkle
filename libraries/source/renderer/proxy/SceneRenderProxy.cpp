#include "renderer/proxy/SceneRenderProxy.h"

#include "core/Container.h"
#include "core/Profiler.h"
#include "core/math/BVH.h"
#include "core/math/Intersection.h"
#include "renderer/BindlessManager.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#include <bvh/v2/default_builder.h>
#include <bvh/v2/executor.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/thread_pool.h>
#pragma GCC diagnostic pop

namespace sparkle
{
class TLAS
{
public:
    explicit TLAS(const std::vector<PrimitiveRenderProxy *> &primitives) : primitives_(primitives)
    {
    }

    void Build()
    {
        auto num_primitives = primitives_.size();
        if (num_primitives == 0)
        {
            return;
        }

        using Vec3 = bvh::v2::Vec<Scalar, 3>;
        using BBox = bvh::v2::BBox<Scalar, 3>;
        bvh::v2::ThreadPool thread_pool;
        bvh::v2::ParallelExecutor executor(thread_pool);

        std::vector<BBox> bboxes(num_primitives);
        std::vector<Vec3> centers(num_primitives);
        executor.for_each(0, num_primitives, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
            {
                auto aabb = primitives_[i]->GetWorldBoundingBox();
                bboxes[i] = BBox(ToBVHVec3(aabb.Min()), ToBVHVec3(aabb.Max()));
                centers[i] = ToBVHVec3(aabb.Center());
            }
        });

        typename bvh::v2::DefaultBuilder<Node>::Config config;
        config.quality = bvh::v2::DefaultBuilder<Node>::Quality::High;
        bvh_ = bvh::v2::DefaultBuilder<Node>::build(thread_pool, bboxes, centers, config);

        std::vector<PrimitiveRenderProxy *> reordered_geometries(num_primitives);
        executor.for_each(0, num_primitives, [this, &reordered_geometries](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
            {
                auto j = bvh_.prim_ids[i];
                reordered_geometries[i] = primitives_[j];
            }
        });

        std::swap(reordered_geometries, primitives_);
    }

    void Intersect(const Ray &ray, Intersection &intersection)
    {
        IntersectInternal<false>(ray, intersection);
    }

    void IntersectAnyHit(const Ray &ray, Intersection &intersection)
    {
        IntersectInternal<true>(ray, intersection);
    }

private:
    template <bool AnyHit> void IntersectInternal(const Ray &ray, Intersection &intersection) const
    {
        if (bvh_.nodes.empty())
        {
            return;
        }

        static constexpr size_t InvalidId = std::numeric_limits<size_t>::max();
        static constexpr size_t StackSize = 32;
        static constexpr bool UseRobustTraversal = false;

        auto prim_id = InvalidId;

        bvh::v2::Ray<Scalar, 3> bvh_ray(ToBVHVec3(ray.Origin()), ToBVHVec3(ray.Direction()));

        IntersectionCandidate candidate;

        bvh::v2::SmallStack<Bvh::Index, StackSize> stack;
        auto leaf_fn = [this, &ray, &prim_id, &candidate](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
            {
                if (AnyHit ? primitives_[i]->IntersectAnyHit(ray, candidate)
                           : primitives_[i]->Intersect(ray, candidate))
                {
                    prim_id = i;
                    candidate.primitive = primitives_[i];
                }
            }
            return prim_id != InvalidId;
        };

        bvh_.intersect<AnyHit, UseRobustTraversal>(bvh_ray, bvh_.get_root().index, stack, leaf_fn);

        if (candidate.primitive)
        {
            if constexpr (AnyHit)
            {
                intersection.Update(ray, candidate.primitive);
            }
            else
            {
                candidate.primitive->GetIntersection(ray, candidate, intersection);
            }
        }
    }

    std::vector<PrimitiveRenderProxy *> primitives_;
    Bvh bvh_;
};

SceneRenderProxy::SceneRenderProxy() = default;

SceneRenderProxy::~SceneRenderProxy() = default;

void SceneRenderProxy::RegisterPrimitive(PrimitiveRenderProxy *primitive)
{
    ASSERT(primitive->GetPrimitiveIndex() == UINT_MAX);

    auto new_index = static_cast<uint32_t>(primitives_.size());
    primitive->SetPrimitiveIndex(new_index);
    primitives_.push_back(primitive);

    primitive_changes_.push_back({.to_id = new_index, .type = PrimitiveChangeType::New});
}

void SceneRenderProxy::UnregisterPrimitive(PrimitiveRenderProxy *primitive)
{
    auto index_to_remove = primitive->GetPrimitiveIndex();

    primitive_changes_.push_back({.from_id = index_to_remove, .type = PrimitiveChangeType::Remove});

    if (RemoveAtSwap(primitives_, index_to_remove))
    {
        auto *swapped_primitive = primitives_[index_to_remove];
        primitive_changes_.push_back({.from_id = swapped_primitive->GetPrimitiveIndex(),
                                      .to_id = index_to_remove,
                                      .type = PrimitiveChangeType::Move});

        swapped_primitive->SetPrimitiveIndex(index_to_remove);
    }

    primitive->SetPrimitiveIndex(UINT_MAX);
}

void SceneRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    PROFILE_SCOPE_LOG("SceneRenderProxy::InitRenderResources");

    RenderProxy::InitRenderResources(rhi, config);

    ASSERT(camera_);

    camera_->InitRenderResources(rhi, config);

    if (sky_proxy_)
    {
        sky_proxy_->InitRenderResources(rhi, config);
    }

    bindless_manager_ = std::make_unique<BindlessManager>(this);

    if (config.IsRayTracingMode())
    {
        bindless_manager_->InitRenderResources(rhi);
    }

    need_bvh_ = config.IsCPURenderMode();
}

void SceneRenderProxy::Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config)
{
    PROFILE_SCOPE("SceneRenderProxy::Update");

    RenderProxy::Update(rhi, camera, config);

    // any change to the scene should trigger canvas re-draw since pixel history makes no sense now
    if (!GetPrimitiveChangeList().empty())
    {
        camera_->MarkPixelDirty();
    }

    for (auto *material : new_materials_)
    {
        material->InitRenderResources(rhi, config);
    }

    for (auto &proxy : proxies_)
    {
        proxy->Update(rhi, camera, config);
    }

    if (bindless_manager_->IsValid())
    {
        bindless_manager_->UpdateFrameData(rhi);
    }

    if (need_bvh_)
    {
        UpdateBVH();
    }
}

void SceneRenderProxy::EndUpdate(RHIContext *rhi)
{
    PROFILE_SCOPE("SceneRenderProxy::EndUpdate");

    primitive_changes_.clear();

    new_materials_.clear();

    auto deferred_deletion_proxies =
        std::make_shared<std::vector<std::unique_ptr<RenderProxy>>>(std::move(deleted_proxies_));
    auto deferred_deletion_materials =
        std::make_shared<std::vector<std::unique_ptr<MaterialRenderProxy>>>(std::move(deleted_materials_));

    rhi->EnqueueEndOfRenderTasks([deferred_deletion_proxies, deferred_deletion_materials]() mutable {
        // now it's safe to delete these resources. this will happen after the frame finishes rendering on GPU.
        // see RHIContext::BeginFrame
        deferred_deletion_proxies->clear();
        deferred_deletion_materials->clear();
    });
}

void SceneRenderProxy::UpdateBVH()
{
    PROFILE_SCOPE("SceneRenderProxy::UpdateBVH");

    for (const auto &[from, to, type] : primitive_changes_)
    {
        switch (type)
        {
        case PrimitiveChangeType::New:
            primitives_[to]->BuildBVH();
            need_bvh_update_ = true;
            break;
        case PrimitiveChangeType::Remove:
        case PrimitiveChangeType::Move:
            need_bvh_update_ = true;
            break;
        case PrimitiveChangeType::Update:
            // TODO(tqjxlm): dynamic primitives
            UnImplemented(type);
            break;
        }
    }

    if (need_bvh_update_)
    {
        tlas_ = std::make_unique<TLAS>(primitives_);
        tlas_->Build();
    }
}

template <bool AnyHit> void SceneRenderProxy::Intersect(const Ray &ray, Intersection &intersection) const
{
    if constexpr (AnyHit)
    {
        tlas_->IntersectAnyHit(ray, intersection);
    }
    else
    {
        tlas_->Intersect(ray, intersection);
    }
}

template void SceneRenderProxy::Intersect<true>(const Ray &ray, Intersection &intersection) const;
template void SceneRenderProxy::Intersect<false>(const Ray &ray, Intersection &intersection) const;

RenderProxy *SceneRenderProxy::AddRenderProxy(std::unique_ptr<RenderProxy> &&proxy)
{
    proxy->SetIndex(static_cast<uint32_t>(proxies_.size()));

    if (proxy->IsPrimitive())
    {
        auto *primitive_proxy = proxy->As<PrimitiveRenderProxy>();
        RegisterPrimitive(primitive_proxy);
    }

    proxies_.emplace_back(std::move(proxy));

    return proxies_.back().get();
}

void SceneRenderProxy::RemoveRenderProxy(RenderProxy *proxy)
{
    auto index = proxy->GetIndex();
    ASSERT(proxies_[index].get() == proxy);

    if (proxy->IsPrimitive())
    {
        auto *primitive_proxy = proxy->As<PrimitiveRenderProxy>();

        UnregisterPrimitive(primitive_proxy);
    }

    // collect deleted proxies for deferred deletion
    deleted_proxies_.emplace_back(std::move(proxies_[index]));

    // maintain a contiguous array
    if (RemoveAtSwap(proxies_, index))
    {
        proxies_[index]->SetIndex(index);
    }
}

MaterialRenderProxy *SceneRenderProxy::AddMaterial(std::unique_ptr<MaterialRenderProxy> &&material)
{
    auto *material_ptr = material.get();

    ASSERT(material_ptr);

    uint32_t material_id;
    if (!free_material_ids_.empty())
    {
        material_id = *free_material_ids_.begin();
        free_material_ids_.erase(material_id);

        ASSERT(!materials_[material_id]);

        materials_[material_id] = std::move(material);
    }
    else
    {
        material_id = static_cast<uint32_t>(materials_.size());

        materials_.push_back(std::move(material));
    }

    new_materials_.insert(material_ptr);

    material_ptr->SetIndex(material_id);

    material_ptr->SetScene(this);

    return material_ptr;
}

void SceneRenderProxy::RemoveMaterial(MaterialRenderProxy *material)
{
    if (!material)
    {
        return;
    }

    auto material_id = material->GetRenderIndex();
    free_material_ids_.insert(material_id);

    // it is possible a material is added and removed in the same frame
    new_materials_.erase(material);

    deleted_materials_.emplace_back(std::move(materials_[material_id]));

    material->SetIndex(UINT_MAX);
}
} // namespace sparkle
