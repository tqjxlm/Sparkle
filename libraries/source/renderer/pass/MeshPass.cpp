#include "renderer/pass/MeshPass.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
void MeshPass::UpdateFrameData(const RenderConfig &, SceneRenderProxy *scene)
{
    if (initialized_)
    {
        for (const auto &[type, primitive, from, to] : scene->GetPrimitiveChangeList())
        {
            switch (type)
            {
            case SceneRenderProxy::PrimitiveChangeType::New:
                HandleNewPrimitive(to);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Remove:
                HandleRemovedPrimitive(from);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Move:
                HandleMovedPrimitive(from, to);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Update:
                HandleUpdatedPrimitive(to);
                break;
            default:
                UnImplemented(type);
                break;
            }
        }
    }
    else
    {
        for (auto *primitive : scene->GetPrimitives())
        {
            HandleNewPrimitive(primitive->GetPrimitiveIndex());
        }

        initialized_ = true;
    }
}

void MeshPass::DrawPrimitives(RGRasterContext &context) const
{
    for (auto *primitive : scene_proxy_->GetPrimitives())
    {
        const auto *proxy = static_cast<const MeshRenderProxy *>(primitive);
        context.DrawMesh(pipeline_states_[primitive->GetPrimitiveIndex()], proxy->GetDrawArgs());
    }
}

void MeshPass::HandleRemovedPrimitive(uint32_t primitive_id)
{
    if (primitive_id < pipeline_states_.size())
    {
        pipeline_states_[primitive_id] = nullptr;
    }
}

void MeshPass::HandleMovedPrimitive(uint32_t from, uint32_t to)
{
    pipeline_states_[to] = std::move(pipeline_states_[from]);
}
} // namespace sparkle
