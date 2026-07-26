#pragma once

#include "scene/component/RenderableComponent.h"

#include <vector>

namespace sparkle
{
class EventSubscription;
class Scene;

class CameraComponent : public RenderableComponent
{
public:
    // values that have physical meaning (reflects real camera attributes)
    struct Attribute
    {
        float focal_length = 0.035f;  // 35mm
        float sensor_height = 0.024f; // full frame
        float aperture = 22.0f;
        float exposure = 1.f;
        float focus_distance = 1.f;

        void Print() const;
    };

    explicit CameraComponent(const Attribute &attribute);

    ~CameraComponent() override;

    // claims the scene input that drives camera posture and attributes. the scene owns the
    // returned subscriptions; every handler resolves the scene's current main camera at
    // dispatch time, so swapping the main camera needs no rebinding.
    [[nodiscard]] static std::vector<std::unique_ptr<EventSubscription>> BindSceneInput(Scene &scene);

    void UpdateRenderData();

    virtual void PrintPosture() = 0;

#pragma region Attributes

    [[nodiscard]] auto GetAttribute() const
    {
        return attribute_;
    }

    void SetFocusDistance(float focus_distance);

    void SetAperture(float aperture);

    void SetExposure(float exposure);

#pragma endregion

#pragma region Input

    virtual void OnPointerDown()
    {
    }

    virtual void OnPointerUp()
    {
    }

    virtual void OnPointerMove(float, float)
    {
    }

    virtual void OnScroll(float)
    {
    }

#pragma endregion

#pragma region Component interfaces

    void OnAttach() override;

#pragma endregion

protected:
    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

private:
    Attribute attribute_;
};
} // namespace sparkle
