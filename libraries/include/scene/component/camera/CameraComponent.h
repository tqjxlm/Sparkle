#pragma once

#include "scene/component/RenderableComponent.h"

namespace sparkle
{
class CameraComponent : public RenderableComponent
{
public:
    // values that have physical meaning (reflects real camera attributes)
    struct CameraAttribute
    {
        float focal_length = 0.035f;  // 35mm
        float sensor_height = 0.024f; // full frame
        float aperture = 22.0f;
        float exposure = 1.f;

        void Print() const;
    };

    // values calculated from physical attributes and used for rendering
    struct CameraState
    {
        float vertical_fov = 0.0f;
        float focus_distance = 0.0f;
        float image_distance = 0.0f;
        float exposure = 1.f;
        float aperture_radius = 0.0f;

        void Update(const CameraAttribute &attribute);

        void Print() const;
    };

    explicit CameraComponent(const CameraAttribute &attribute);

    ~CameraComponent() override;

    void UpdateRenderData();

    virtual void PrintPosture() = 0;

#pragma region Attributes

    [[nodiscard]] const CameraAttribute &GetAttribute() const
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
    CameraAttribute attribute_;
    CameraState state_;
};
} // namespace sparkle
