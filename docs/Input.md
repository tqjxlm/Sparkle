# Input Handling

All user input flows through a single pipeline owned by `InputManager` ([libraries/include/application/InputManager.h](../libraries/include/application/InputManager.h)):

```text
platform layer                InputManager                       subscribers
──────────────                ─────────────────────────────      ─────────────────
glfw callbacks     ┐          Push(InputEvent)  → queue           camera, renderer,
macos NSEvents     ├─ raw ──► DispatchPendingEvents()             scene, app, …
ios raw touches    │          ├─ feed imgui (io.Add*Event)        (each module claims
android motions    │          ├─ ui capture gate                   the inputs it
tests (injection)  ┘          ├─ map to scene events        ──►     implements)
                              └─ dispatch key bindings      ──►
```

* **Raw events** ([InputEvents.h](../libraries/include/application/InputEvents.h)): `PointerEvent` (mouse and touch unified: device, action, pointer id, button, modifiers, position), `ScrollEvent`, `KeyEvent`, `CharEvent`. They cross only the platform boundary — no module outside `InputManager` handles them.
* **Scene events** (subscribe via `Event<Args...>` / RAII `EventSubscription`, [core/Event.h](../libraries/include/core/Event.h)) are recognized gestures, not pointers. They are broadcast: any number of modules may observe one.
* **Key bindings** (`InputManager::BindKey`): a `KeyBinding` is a key, a `KeyAction`, and the modifiers that must be held (extra modifiers do not block it). A binding is claimed in an `InputLayer` and freed when the returned subscription dies.

Events are queued by `Push` (main thread only) and delivered once per frame from the main loop, in push order — input handling is deterministic with respect to frames.

## Scene gestures

Recognition — which button, which modifier, how many fingers, how long — happens once inside `InputManager`, so no subscriber re-derives it and mouse and touch reach a subscriber through the same event:

| scene event | mouse | touch |
| --- | --- | --- |
| `OnSceneDragBegin` / `OnSceneDrag(delta)` / `OnSceneDragEnd` | primary button pressed and moving | one finger |
| `OnSceneZoom(amount)` | wheel | pinch |
| `OnSceneSecondaryClick(position)` | secondary button, or control + primary | — |
| `OnSceneTap(finger_count)` | — | quick tap, no slop |
| `OnSceneDoubleTap(finger_count)` | double click | double tap |

A drag always ends: `OnSceneDragEnd` fires on release and also when the ui takes the sequence over mid-drag, so an owner that started acting on `Begin` can rely on `End` to stop.

## Key arbitration

A key often means different things to different modules — `Escape` dismisses the config panel, and it also exits the app. `InputLayer` resolves that without either module knowing about the other:

* Layers answer a key in declaration order (`Ui`, then `Scene`), and a handler returns `true` to consume the key, which stops lower layers from seeing it.
* An owner that is currently irrelevant — a panel that is closed — returns `false`, and the key falls through to the layer below as if the binding did not exist.
* Within one layer a key has exactly one owner; claiming a key a second time in the same layer asserts. Two owners in one layer have no defined order, whereas two owners in different layers do.

Everything a key can reach is therefore visible from its bindings, and a new consumer of an already-bound key is added by picking a layer rather than by editing the module that already owns it.

While imgui wants the keyboard (`IsHandlingKeyboardEvent`, e.g. a focused text field) no binding runs in any layer — imgui itself is the owner of the keystroke.

## Ownership

Every input is implemented and claimed by the module that owns the state it changes, not by a central dispatcher. `InputManager::Instance()` gives a module access without any wiring from the app layer; it is null in cook mode, so a module must tolerate its absence.

| input | layer | owner |
| --- | --- | --- |
| drag, zoom, `Up`/`Down` (aperture), `P` (print posture) | `Scene` | `CameraComponent::BindSceneInput`, resolving the scene's current main camera |
| `Space` hold (manual accumulation) | `Scene` | `RenderConfig::BindInput` |
| secondary click (debug point) | — | `RenderFramework` |
| `NumpadAdd` / `Shift`+`Equal` / `Minus` (debug spheres) | `Scene` | `SceneManager::BindDebugInput` |
| `Escape` (dismiss the config panel) | `Ui` | `AppFramework` |
| `Escape` (exit), double tap (config panel), 4-finger tap (frame capture) | `Scene` | `AppFramework` |

Scene-owned bindings live for the lifetime of the `Scene` and resolve the state they act on at dispatch time, so loading another scene never rebinds them.

Because a gesture is recognized once, the handlers a module writes are the whole of what it needs: `CameraComponent`'s `OnDragBegin` / `OnDrag` / `OnDragEnd` / `OnZoom` are protected, driven only by the bindings the camera itself claims.

## Coordinate space

Pointer positions are in **ui space**: the space of imgui's `io.DisplaySize` — window points on desktop, render-target pixels on mobile.

## UI capture

imgui receives every pointer event through the same pipeline (`io.Add*Event`); no platform imgui backend handles pointer input. A pointer sequence that starts on the ui stays on the ui until release (`InputManager::UiCaptureGate`), and scene events are suppressed while imgui wants the pointer or keyboard — on every platform, touch included.

Desktop keyboards: glfw chains the imgui glfw backend's key/char callbacks for full key coverage; macos maps `NativeKeyboard` unichars to `ImGuiKey` and feeds printable characters as `CharEvent`.

## Touch gestures

Gesture recognition is shared engine code, identical on ios and android (platforms only convert native touches to `PointerEvent`s):

| gesture | scene event |
| --- | --- |
| 1-finger drag | `OnSceneDragBegin` / `OnSceneDrag` / `OnSceneDragEnd` |
| pinch (2 fingers) | `OnSceneZoom` from the pinch length delta; suppresses drag; lifting back to one finger resumes the drag |
| quick tap, no slop | `OnSceneTap(finger_count)`; 1-finger taps also drive double-tap detection (4-finger tap triggers a frame capture) |
| touch → ui | single finger emulates an imgui mouse: short = click, longer drag = wheel scroll, hover reset after each sequence |

## Testing and mocking

Tests inject events through the exact entry point platforms use:

```cpp
app.PushInputEvent(PointerEvent{.action = PointerAction::Down, .position = {400.f, 300.f}});
```

The `input_injection` test case (part of the `dev/run_tests.py` suite, see [Test.md](Test.md)) drives mouse and touch sequences this way. Because gesture recognition is platform-neutral, the mobile input path is exercised on desktop CI.
