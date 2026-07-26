#include "scene/component/camera/CameraComponent.h"

#include "application/InputManager.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"

namespace sparkle
{
// one f-stop per key press
constexpr float ApertureStep = 1.f;

CameraComponent::CameraComponent(const Attribute &attribute) : attribute_(attribute)
{
}

CameraComponent::~CameraComponent() = default;

std::vector<std::unique_ptr<EventSubscription>> CameraComponent::BindSceneInput(Scene &scene)
{
    auto *input_manager = InputManager::Instance();
    if (!input_manager)
    {
        return {};
    }

    std::vector<std::unique_ptr<EventSubscription>> subscriptions;

    subscriptions.push_back(input_manager->OnScenePointer().Subscribe([&scene](const PointerEvent &event) {
        auto *camera = scene.GetMainCamera();
        if (!camera || event.button != ClickButton::PrimaryLeft)
        {
            return;
        }

        switch (event.action)
        {
        case PointerAction::Down:
            // control + primary is the debug-point gesture, so it must not start a camera drag
            if ((event.modifiers & static_cast<uint32_t>(KeyboardModifier::Control)) == 0)
            {
                camera->OnPointerDown();
            }
            break;
        case PointerAction::Up:
        case PointerAction::Cancel:
            camera->OnPointerUp();
            break;
        default:
            break;
        }
    }));

    subscriptions.push_back(input_manager->OnSceneDrag().Subscribe([&scene](Vector2 delta) {
        if (auto *camera = scene.GetMainCamera())
        {
            camera->OnPointerMove(delta.y(), -delta.x());
        }
    }));

    subscriptions.push_back(input_manager->OnSceneZoom().Subscribe([&scene](float amount) {
        if (auto *camera = scene.GetMainCamera())
        {
            camera->OnScroll(amount);
        }
    }));

    auto bind_aperture_step = [input_manager, &scene, &subscriptions](Key key, float step) {
        subscriptions.push_back(input_manager->BindKey({.key = key}, [&scene, step]() {
            if (auto *camera = scene.GetMainCamera())
            {
                camera->SetAperture(camera->GetAttribute().aperture + step);
            }
        }));
    };

    bind_aperture_step(Key::Up, ApertureStep);
    bind_aperture_step(Key::Down, -ApertureStep);

    subscriptions.push_back(input_manager->BindKey({.key = Key::P}, [&scene]() {
        if (auto *camera = scene.GetMainCamera())
        {
            camera->PrintPosture();
        }
    }));

    return subscriptions;
}

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
