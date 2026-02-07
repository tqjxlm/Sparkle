#pragma once

#include "core/math/Types.h"

#include <optional>

namespace sparkle
{
class CameraComponent;
class Scene;

class SessionManager
{
public:
    struct CameraState
    {
        Vector3 translation = Zeros;
        Vector4 rotation = Vector4(0.f, 0.f, 0.f, 1.f);
        Vector3 scale = Ones;
    };

    static void SaveSession(CameraComponent *camera);

    void SetLoadLastSession(bool load_last_session);

    bool LoadLastSession();

    void LoadLastSessionIfRequested();

    void ApplyCamera(CameraComponent *camera);

    void DrawUi(Scene *scene, bool need_default_sky, bool need_default_lighting);

private:
    bool LoadLastSessionInternal();

    bool load_last_session_ = false;
    std::optional<CameraState> pending_camera_;
};
} // namespace sparkle
