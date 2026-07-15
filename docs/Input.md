# Input Handling

All user input flows through a single pipeline owned by `InputManager` ([libraries/include/application/InputManager.h](../libraries/include/application/InputManager.h)):

```text
platform layer                InputManager                       subscribers
──────────────                ─────────────────────────────      ─────────────────
glfw callbacks     ┐          Push(InputEvent)  → queue
macos NSEvents     ├─ raw ──► DispatchPendingEvents()            AppFramework
ios raw touches    │          ├─ feed imgui (io.Add*Event)       (camera routing,
android motions    │          ├─ ui capture gate                  app shortcuts,
tests (injection)  ┘          └─ map to scene events        ──►   panel toggle, …)
```

* **Raw events** ([InputEvents.h](../libraries/include/application/InputEvents.h)): `PointerEvent` (mouse and touch unified: device, action, pointer id, button, modifiers, position), `ScrollEvent`, `KeyEvent`, `CharEvent`.
* **Scene events** (subscribe via `Event<Args...>` / RAII `EventSubscription`, [core/Event.h](../libraries/include/core/Event.h)): `OnScenePointer` (gated raw pointers, incl. synthesized `Cancel` when the ui takes over a sequence), `OnSceneDrag` (delta while the primary pointer is pressed), `OnSceneZoom` (wheel and pinch unified), `OnSceneTap(finger_count)`, `OnSceneDoubleTap`, `OnSceneKey`.

Events are queued by `Push` (main thread only) and delivered once per frame from the main loop, in push order — input handling is deterministic with respect to frames.

## Coordinate space

Pointer positions are in **ui space**: the space of imgui's `io.DisplaySize` — window points on desktop, render-target pixels on mobile.

## UI capture

imgui receives every pointer event through the same pipeline (`io.Add*Event`); no platform imgui backend handles pointer input. A pointer sequence that starts on the ui stays on the ui until release (`InputManager::UiCaptureGate`), and scene events are suppressed while imgui wants the pointer or keyboard — on every platform, touch included.

Desktop keyboards: glfw chains the imgui glfw backend's key/char callbacks for full key coverage; macos maps `NativeKeyboard` unichars to `ImGuiKey` and feeds printable characters as `CharEvent`.

## Touch gestures

Gesture recognition is shared engine code, identical on ios and android (platforms only convert native touches to `PointerEvent`s):

| gesture | scene event |
| --- | --- |
| 1-finger drag | `OnScenePointer` Down/Up + `OnSceneDrag` |
| pinch (2 fingers) | `OnSceneZoom` from the pinch length delta; suppresses drag; lifting back to one finger resumes the drag |
| quick tap, no slop | `OnSceneTap(finger_count)`; 1-finger taps also drive double-tap detection (4-finger tap triggers a frame capture) |
| touch → ui | single finger emulates an imgui mouse: short = click, longer drag = wheel scroll, hover reset after each sequence |

## Testing and mocking

Tests inject events through the exact entry point platforms use:

```cpp
app.PushInputEvent(PointerEvent{.action = PointerAction::Down, .position = {400.f, 300.f}});
```

The `input_injection` test case (part of the `dev/run_tests.py` suite, see [Test.md](Test.md)) drives mouse and touch sequences this way. Because gesture recognition is platform-neutral, the mobile input path is exercised on desktop CI.
