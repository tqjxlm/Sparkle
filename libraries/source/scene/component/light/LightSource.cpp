#include "scene/component/light/LightSource.h"

#include "core/task/TaskManager.h"

namespace sparkle
{
LightSourceComponent::LightSourceComponent() = default;

LightSourceComponent::~LightSourceComponent() = default;

void LightSourceComponent::OnAttach()
{
    RenderableComponent::OnAttach();

    TaskManager::RunInRenderThread([this]() { RecreateRenderProxy(); });
}
} // namespace sparkle
