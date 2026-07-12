#include "application/AppFramework.h"
#include "application/InputManager.h"
#include "application/NativeKeyboard.h"
#include "application/TestCase.h"
#include "core/Event.h"
#include "core/Logger.h"
#include "scene/component/camera/OrbitCameraComponent.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace sparkle
{
// injects synthetic input events through InputManager::Push and verifies the observable
// outcome one frame later, after the main loop dispatches them.
class InputInjectionTest : public TestCase
{
    struct Step
    {
        const char *name;
        std::function<void(AppFramework &)> inject;
        std::function<bool(AppFramework &)> verify;
    };

public:
    Result OnTick(AppFramework &app) override
    {
        if (steps_.empty())
        {
            if (!VerifyCaptureGate())
            {
                Log(Error, "[{}] ui capture gate state machine misbehaves", GetName());
                return Result::Fail;
            }

            camera_ = dynamic_cast<OrbitCameraComponent *>(app.GetMainCamera());
            if (!camera_)
            {
                Log(Error, "[{}] main camera is not an OrbitCameraComponent", GetName());
                return Result::Fail;
            }

            BuildSteps();
        }

        if (pending_verify_)
        {
            pending_verify_ = false;
            const auto &step = steps_[current_step_];
            if (!step.verify(app))
            {
                Log(Error, "[{}] step '{}' failed", GetName(), step.name);
                return Result::Fail;
            }
            Log(Info, "[{}] step '{}' passed", GetName(), step.name);
            current_step_++;
        }

        if (current_step_ >= steps_.size())
        {
            return Result::Pass;
        }

        steps_[current_step_].inject(app);
        pending_verify_ = true;

        return Result::Pending;
    }

private:
    static bool VerifyCaptureGate()
    {
        InputManager::UiCaptureGate gate;

        // a sequence starting on the ui is consumed until release, even off the ui
        bool ok = gate.ShouldConsume(PointerAction::Down, true);
        ok = ok && gate.IsSequenceActive();
        ok = ok && gate.ShouldConsume(PointerAction::Move, false);
        ok = ok && gate.ShouldConsume(PointerAction::Up, false);

        // afterwards the scene receives events again
        ok = ok && !gate.IsSequenceActive();
        ok = ok && !gate.ShouldConsume(PointerAction::Move, false);

        // a sequence starting on the scene stays on the scene
        ok = ok && !gate.ShouldConsume(PointerAction::Down, false);
        ok = ok && !gate.IsSequenceActive();
        ok = ok && !gate.ShouldConsume(PointerAction::Move, false);
        ok = ok && !gate.ShouldConsume(PointerAction::Up, false);

        return ok;
    }

    static bool Differs(float a, float b)
    {
        return std::abs(a - b) > 1e-5f;
    }

    static void InjectClick(AppFramework &app, const Vector2 &position)
    {
        app.PushInputEvent(PointerEvent{.action = PointerAction::Down, .position = position});
        app.PushInputEvent(PointerEvent{.action = PointerAction::Up, .position = position});
    }

    void BuildSteps()
    {
        const Vector2 start{400.f, 300.f};
        const Vector2 end{420.f, 320.f};

        steps_.push_back({.name = "drag rotates the camera",
                          .inject =
                              [=, this](AppFramework &app) {
                                  yaw_before_ = camera_->GetYaw();
                                  pitch_before_ = camera_->GetPitch();
                                  app.PushInputEvent(PointerEvent{.action = PointerAction::Down, .position = start});
                                  app.PushInputEvent(PointerEvent{.action = PointerAction::Move, .position = end});
                                  app.PushInputEvent(PointerEvent{.action = PointerAction::Up, .position = end});
                              },
                          .verify =
                              [this](AppFramework &) {
                                  return Differs(camera_->GetYaw(), yaw_before_) &&
                                         Differs(camera_->GetPitch(), pitch_before_);
                              }});

        steps_.push_back({.name = "scroll zooms the camera",
                          .inject =
                              [this](AppFramework &app) {
                                  radius_before_ = camera_->GetRadius();
                                  app.PushInputEvent(ScrollEvent{.delta = Vector2(0.f, 2.f)});
                              },
                          .verify = [this](AppFramework &) { return Differs(camera_->GetRadius(), radius_before_); }});

        steps_.push_back({.name = "double click toggles the control panel",
                          .inject =
                              [=](AppFramework &app) {
                                  InjectClick(app, start);
                                  InjectClick(app, start);
                              },
                          .verify = [](AppFramework &app) { return app.IsControlPanelVisible(); }});

        steps_.push_back({.name = "arrow key changes the aperture",
                          .inject =
                              [this](AppFramework &app) {
                                  // the default aperture sits at the clamp maximum, so step downwards
                                  aperture_before_ = camera_->GetAttribute().aperture;
                                  const int key = static_cast<int>(NativeKeyboard::KeyDown);
                                  app.PushInputEvent(KeyEvent{.key = key, .action = KeyAction::Press});
                                  app.PushInputEvent(KeyEvent{.key = key, .action = KeyAction::Release});
                              },
                          .verify =
                              [this](AppFramework &) {
                                  return !Differs(camera_->GetAttribute().aperture, aperture_before_ - 1.f);
                              }});

        steps_.push_back({.name = "space holds accumulation",
                          .inject =
                              [](AppFramework &app) {
                                  app.PushInputEvent(KeyEvent{.key = static_cast<int>(NativeKeyboard::KeySpace),
                                                              .action = KeyAction::Press});
                              },
                          .verify = [](AppFramework &app) { return app.GetRenderConfig().accumulate_key_held; }});

        steps_.push_back({.name = "space release stops accumulation",
                          .inject =
                              [](AppFramework &app) {
                                  app.PushInputEvent(KeyEvent{.key = static_cast<int>(NativeKeyboard::KeySpace),
                                                              .action = KeyAction::Release});
                              },
                          .verify = [](AppFramework &app) { return !app.GetRenderConfig().accumulate_key_held; }});

        auto touch = [](PointerAction action, uint8_t id, const Vector2 &position) {
            return PointerEvent{.device = PointerDevice::Touch, .action = action, .id = id, .position = position};
        };

        steps_.push_back({.name = "touch drag rotates the camera",
                          .inject =
                              [=, this](AppFramework &app) {
                                  yaw_before_ = camera_->GetYaw();
                                  pitch_before_ = camera_->GetPitch();
                                  app.PushInputEvent(touch(PointerAction::Down, 0, start));
                                  app.PushInputEvent(touch(PointerAction::Move, 0, end));
                                  app.PushInputEvent(touch(PointerAction::Up, 0, end));
                              },
                          .verify =
                              [this](AppFramework &) {
                                  return Differs(camera_->GetYaw(), yaw_before_) &&
                                         Differs(camera_->GetPitch(), pitch_before_);
                              }});

        steps_.push_back({.name = "pinch zooms the camera without rotating it",
                          .inject =
                              [=, this](AppFramework &app) {
                                  yaw_before_ = camera_->GetYaw();
                                  radius_before_ = camera_->GetRadius();
                                  app.PushInputEvent(touch(PointerAction::Down, 0, {400.f, 300.f}));
                                  app.PushInputEvent(touch(PointerAction::Down, 1, {500.f, 300.f}));
                                  app.PushInputEvent(touch(PointerAction::Move, 1, {550.f, 300.f}));
                                  app.PushInputEvent(touch(PointerAction::Up, 1, {550.f, 300.f}));
                                  app.PushInputEvent(touch(PointerAction::Up, 0, {400.f, 300.f}));
                              },
                          .verify =
                              [this](AppFramework &) {
                                  return Differs(camera_->GetRadius(), radius_before_) &&
                                         !Differs(camera_->GetYaw(), yaw_before_);
                              }});

        steps_.push_back({.name = "two-finger tap fires a tap event",
                          .inject =
                              [=, this](AppFramework &app) {
                                  last_tap_fingers_ = 0;
                                  tap_subscription_ = app.GetInputManager()->OnSceneTap().Subscribe(
                                      [this](uint8_t finger_count) { last_tap_fingers_ = finger_count; });
                                  app.PushInputEvent(touch(PointerAction::Down, 0, {400.f, 300.f}));
                                  app.PushInputEvent(touch(PointerAction::Down, 1, {450.f, 300.f}));
                                  // landing jitter below the tap slop must not defeat the tap
                                  app.PushInputEvent(touch(PointerAction::Move, 0, {403.f, 302.f}));
                                  app.PushInputEvent(touch(PointerAction::Move, 1, {452.f, 298.f}));
                                  app.PushInputEvent(touch(PointerAction::Up, 0, {403.f, 302.f}));
                                  app.PushInputEvent(touch(PointerAction::Up, 1, {452.f, 298.f}));
                              },
                          .verify = [this](AppFramework &) { return last_tap_fingers_ == 2; }});

        steps_.push_back(
            {.name = "touch double tap toggles the control panel",
             .inject =
                 [=, this](AppFramework &app) {
                     // sit out the double-click cooldown left by earlier steps
                     std::this_thread::sleep_for(std::chrono::milliseconds(350));
                     panel_before_ = app.IsControlPanelVisible();
                     app.PushInputEvent(touch(PointerAction::Down, 0, start));
                     app.PushInputEvent(touch(PointerAction::Up, 0, start));
                     app.PushInputEvent(touch(PointerAction::Down, 0, start));
                     app.PushInputEvent(touch(PointerAction::Up, 0, start));
                 },
             .verify = [this](AppFramework &app) { return app.IsControlPanelVisible() != panel_before_; }});
    }

    std::vector<Step> steps_;
    size_t current_step_ = 0;
    bool pending_verify_ = false;

    OrbitCameraComponent *camera_ = nullptr;
    float yaw_before_ = 0.f;
    float pitch_before_ = 0.f;
    float radius_before_ = 0.f;
    float aperture_before_ = 0.f;
    bool panel_before_ = false;
    uint8_t last_tap_fingers_ = 0;
    std::unique_ptr<EventSubscription> tap_subscription_;
};

static TestCaseRegistrar<InputInjectionTest> input_injection_test_registrar("input_injection");
} // namespace sparkle
