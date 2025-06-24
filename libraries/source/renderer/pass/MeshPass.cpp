#include "renderer/pass/MeshPass.h"

#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
void MeshPass::UpdateFrameData(const RenderConfig &, SceneRenderProxy *scene)
{
    if (initialized_)
    {
        for (const auto &[from, to, type] : scene->GetPrimitiveChangeList())
        {
            switch (type)
            {
            case SceneRenderProxy::PrimitiveChangeType::New:
                HandleNewPrimitive(to);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Remove:
                // TODO(tqjxlm): handle removed primitives
                HandleRemovedPrimitive(from);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Move:
                HandleMovedPrimitive(from, to);
                break;
            case SceneRenderProxy::PrimitiveChangeType::Update:
                HandleUpdatedPrimitive(to);
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

void MeshPass::Render()
{
    for (auto *primitive : scene_proxy_->GetPrimitives())
    {
        auto *proxy = static_cast<MeshRenderProxy *>(primitive);
        proxy->Render(rhi_, pipeline_states_[primitive->GetPrimitiveIndex()]);
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
    ASSERT(pipeline_states_[to] == nullptr);

    pipeline_states_[to] = std::move(pipeline_states_[from]);
}
} // namespace sparkle
