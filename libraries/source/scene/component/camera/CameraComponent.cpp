#include "scene/component/camera/CameraComponent.h"

#include "core/Logger.h"
#include "core/TaskManager.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"

namespace sparkle
{
CameraComponent::CameraComponent(const CameraAttribute &attribute) : attribute_(attribute)
{
}

CameraComponent::~CameraComponent() = default;

void CameraComponent::SetExposure(float exposure)
{
    // TODO(tqjxlm): exposure as in photography terminology
    attribute_.exposure = exposure;
    state_.exposure = attribute_.exposure;

    UpdateRenderData();
}

void CameraComponent::SetAperture(float aperture)
{
    auto new_aperture = std::clamp(aperture, 0.95f, 22.f);
    if (std::abs(new_aperture - attribute_.aperture) < Eps)
    {
        return;
    }

    attribute_.aperture = new_aperture;

    state_.Update(attribute_);

    UpdateRenderData();
}

void CameraComponent::SetFocusDistance(float focus_distance)
{
    // assume fixed focal length
    state_.focus_distance = focus_distance;

    state_.Update(attribute_);

    UpdateRenderData();
}

std::unique_ptr<RenderProxy> CameraComponent::CreateRenderProxy()
{
    auto proxy = std::make_unique<CameraRenderProxy>();

    state_.Update(attribute_);

    proxy->SetData(state_);

    node_->GetScene()->GetRenderProxy()->SetCamera(proxy.get());

    return proxy;
}

void CameraComponent::OnAttach()
{
    Component::OnAttach();

    TaskManager::RunInRenderThread([this]() { RecreateRenderProxy(); });
}

void CameraComponent::UpdateRenderData()
{
    state_.Update(attribute_);

    if (GetRenderProxy())
    {
        TaskManager::RunInRenderThread(
            [this, state = state_]() { GetRenderProxy()->As<CameraRenderProxy>()->SetData(state); });
    }
}

void CameraComponent::CameraState::Print() const
{
    Log(Info, "camera state: vertical_fov {} focus_distance {} image_distance {} aperture_radius {}", vertical_fov,
        focus_distance, image_distance, aperture_radius);
}

void CameraComponent::CameraAttribute::Print() const
{
    Log(Info, "camera attribute: focal_length {} sensor_height {} aperture {} exposure {}", focal_length, sensor_height,
        aperture, exposure);
}

void CameraComponent::CameraState::Update(const CameraAttribute &attribute)
{
    vertical_fov = 2.f * std::atan(attribute.sensor_height / (2.f * attribute.focal_length));
    aperture_radius = attribute.focal_length / attribute.aperture * 0.5f;
    image_distance = 1.f / (1.f / attribute.focal_length - 1.f / focus_distance);
}
} // namespace sparkle
