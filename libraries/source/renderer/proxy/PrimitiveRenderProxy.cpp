#include "renderer/proxy/PrimitiveRenderProxy.h"

namespace sparkle
{
PrimitiveRenderProxy::PrimitiveRenderProxy(std::string_view name, AABB local_bound)
    : local_bound_(std::move(local_bound)), name_(std::move(name))
{
    ASSERT(!name.empty());

    is_primitive_ = true;
}

PrimitiveRenderProxy::~PrimitiveRenderProxy() = default;

void PrimitiveRenderProxy::Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config)
{
    RenderProxy::Update(rhi, camera, config);

    if (transform_dirty_)
    {
        OnTransformDirty(rhi);
    }
}

void PrimitiveRenderProxy::OnTransformDirty(RHIContext *rhi)
{
    RenderProxy::OnTransformDirty(rhi);
    world_bound_ = local_bound_.TransformTo(transform_);
}
} // namespace sparkle
