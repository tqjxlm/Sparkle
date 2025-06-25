#include "scene/component/camera/CameraComponent.h"

#include "core/Logger.h"
#include "core/TaskManager.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"

namespace sparkle
{
CameraComponent::CameraComponent(const Attribute &attribute) : attribute_(attribute)
{
}

CameraComponent::~CameraComponent() = default;

void CameraComponent::SetExposure(float exposure)
{
    // TODO(tqjxlm): exposure as in photography terminology
    attribute_.exposure = exposure;

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

    UpdateRenderData();
}

void CameraComponent::SetFocusDistance(float focus_distance)
{
    // assume fixed focal length
    attribute_.focus_distance = focus_distance;

    UpdateRenderData();
}

// translate physical attributes to render attributes
static auto CalculateRenderAttribute(const CameraComponent::Attribute &attribute)
{
    return CameraRenderProxy::Attribute{
        .vertical_fov = 2.f * std::atan(attribute.sensor_height / (2.f * attribute.focal_length)),
        .focus_distance = attribute.focus_distance,
        .exposure = attribute.exposure,
        .aperture_radius = attribute.focal_length / attribute.aperture * 0.5f,
    };
}

std::unique_ptr<RenderProxy> CameraComponent::CreateRenderProxy()
{
    auto proxy = std::make_unique<CameraRenderProxy>();

    proxy->UpdateAttribute(CalculateRenderAttribute(attribute_));

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
    auto render_attrib = CalculateRenderAttribute(attribute_);

    if (GetRenderProxy())
    {
        TaskManager::RunInRenderThread(
            [this, render_attrib]() { GetRenderProxy()->As<CameraRenderProxy>()->UpdateAttribute(render_attrib); });
    }
}

void CameraComponent::Attribute::Print() const
{
    Log(Info, "camera attribute: focal_length {} sensor_height {} aperture {} exposure {}", focal_length, sensor_height,
        aperture, exposure);
}
} // namespace sparkle
