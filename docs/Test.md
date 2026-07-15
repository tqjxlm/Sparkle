# Test Guide

## Visual Test

* Use `--test_case screenshot` to take a screenshot after the scene is fully loaded and a frame is fully rendered.
* Use `--test_case multi_frame_screenshot` to take 5 frames of screenshots after the scene is fully loaded and a frame is fully rendered. This is useful for temporal analysis.
* Screenshot test cases work with `--headless true`, so it is suitable for commandline use.
* Screenshots are saved to [external-storage-path]/screenshots/. For `--test_case screenshot`, the file is named `screenshot.png`. For `--test_case multi_frame_screenshot`, files are named `multi_frame_N.png` where `N` is the frame index. The UI "Save Screenshot" button names files with the scene name, pipeline, and timestamp. For [external-storage-path], refer to [Run.md](Run.md).
* Ground truth images can be found in [CI.md](CI.md). But if you are working on a feature that is meant to change the final image output, you should not rely on the ground truth images.

### Confirm you are looking at the right output first

* With **no `--scene`**, the app loads the packaged **TestScene** (`resources/packed/TestScene.usda`: a floor, several spheres, and glTF models against a sky map) — the scene the CI ground truth is rendered from. `--scene <path>` loads a model/scene **file path** instead. A name that is not a real path (e.g. `--scene TestScene`) silently logs an error, renders an **empty default-sky scene**, and still exits `0`. Before analysing anything, **verify the expected geometry is actually in the frame** — it is easy to spend a long time analysing a blank or wrong render.
* Likewise confirm the intended pipeline / debug view / cvars actually took effect (check the image, not just the command line).

### Comparing and diffing renders

How to compare a render against the ground truth — signed per-pixel diffs, 1:1 crops, scanline profiles, and why aggregate metrics (FLIP / PSNR) or blurred diffs must never be the detector — is methodology, covered in [RenderingValidation.md](RenderingValidation.md).

## Python Test Scripts

* When possible, always use python scripts to perform tests.
* Add Python test dependencies to `tests/requirements.txt`.
* `dev/run_tests.py` is the single general test orchestrator. Do not add another script that builds or sequences the general test suite.
* Focused test implementations, evaluators, and reusable test helpers belong under `tests/`, next to the feature they validate.
* If Python is not sufficient, combine a focused script under `tests/` with the TestCase system below.

## Test Orchestration

`dev/run_tests.py` runs the Python unit tests as a fail-fast preflight, builds once, runs both screenshot pipelines (forward and deferred) and evaluates them against the ground truth, runs the camera nudge round trip and the USD round trip, and executes the `cooker_request`, `scene_load_failure`, `render_target_pool` and `pipeline_switch_pool` test cases; on the macos framework with a physical GPU it also runs `ibl_parity`. The application suite runs to completion and reports all failures together.

```bash
python3 dev/run_tests.py --framework macos --config Release --headless
python3 dev/run_tests.py --framework glfw --config Release --headless
```

Use `--pipeline <name>` to focus screenshot coverage while developing; repeat the option for multiple pipelines. Use `--skip_build` only when the intended binary is already built. CI runs the packaged Windows build under Lavapipe with `--software --require_cooked`, and the packaged macOS build on a physical Metal GPU with `--require_cooked`, which rejects runtime cooking. See [CI.md](CI.md) for the exact commands and why the gpu pipeline stays local-only.

The static-render evaluator lives at [tests/screenshot/static_render_test.py](../tests/screenshot/static_render_test.py), and the USD evaluator lives at [tests/usd/usd_roundtrip_test.py](../tests/usd/usd_roundtrip_test.py). They own specialized assertions; they do not orchestrate the general suite. Shared framework, dependency and image mechanics live in [tests/rendering/render_test_support.py](../tests/rendering/render_test_support.py), so one evaluator never serves as another evaluator's utility layer.

## TestCase System

* A `TestCase` is a C++ class that runs inside the app after the scene finishes loading.
* Before render and RHI config init, the selected test case may override config values via `OnEnforceConfigs()`.
* Each frame, `AppFramework` calls `Tick()`. When `Tick()` returns `Pass` or `Fail` the app exits with code `0` or `1` respectively.

### Build

TestCase support is compiled in by default (`ENABLE_TEST_CASES=1`). Use `--strip_test` to exclude it from the binary for production builds:

```bash
# Build without TestCase support (production)
python3 build.py --framework [glfw, macos] --strip_test
```

### Run

* Test cases are selected at runtime via the `test_case` cvar.
* It is usually recommended to run test cases in headless mode.

```bash
# Build & Run the smoke test (exits 0 on pass, 1 on fail)
python3 build.py --framework [glfw, macos] --run --test_case smoke --headless true
echo "Exit code: $?"
```

The `--test_case <name>` argument is passed straight through to the app's config system. Any other cvars (e.g. `--scene`, `--pipeline`) work alongside it as normal.

Use `--test_timeout <frames>` to set a frame budget for the test. If the test does not finish within the given number of frames it is reported as `Fail`. Default is `0` (no limit).

### Exit Codes

| Code | Meaning                                           |
| ---- | ------------------------------------------------- |
| `0`  | `TestCase::Result::Pass` — test passed            |
| `1`  | `TestCase::Result::Fail` — test failed            |
| `1`  | Named test case was not registered (init failure) |

## Writing a Test Case

1. Create a `.cpp` file anywhere under `tests/`.
2. Subclass `sparkle::TestCase` and implement `OnTick()`.
3. If the test needs fixed runtime config, optionally override `OnEnforceConfigs()`.
4. Register with `TestCaseRegistrar<T>` using a unique name string.

The registered name is injected into the test instance and available via `TestCase::GetName()`, so log messages do not need to duplicate it manually.

```cpp
// tests/my_feature/MyFeatureTest.cpp
#include "application/AppFramework.h"
#include "application/TestCase.h"

namespace sparkle
{
class MyFeatureTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
    }

    Result OnTick(AppFramework &app) override
    {
        // Inspect app state each frame. Return Pending to keep running.
        if (frame_ < 10)
        {
            return Result::Pending;
        }
        // Evaluate your condition here.
        return Result::Pass;
    }
};

static TestCaseRegistrar<MyFeatureTest> my_feature_test_registrar("my_feature");
} // namespace sparkle
```

Then run it:

```bash
python3 build.py --framework macos --run --test_case my_feature
```

### Naming Rules

Test case names (the string passed to `TestCaseRegistrar`) must be unique across all `.cpp` files under `tests/`. Duplicate names are detected at startup and logged as an error; the first registration wins.

## Built-in Test Cases

| Name                     | File                                                                                              | What it does                                                                                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `smoke`                  | [tests/smoke/SmokeTest.cpp](../tests/smoke/SmokeTest.cpp)                                         | Waits 2 frames then returns `Pass`. Verifies the full init and scene-load pipeline.                                                                             |
| `screenshot`             | [tests/screenshot/ScreenshotTest.cpp](../tests/screenshot/ScreenshotTest.cpp)                     | Waits for renderer ready, clears all existing screenshots optionally, captures one, then passes. Used by functional tests and visual QA.                        |
| `multi_frame_screenshot` | [tests/screenshot/MultiFrameScreenshotTest.cpp](../tests/screenshot/MultiFrameScreenshotTest.cpp) | Waits for renderer ready, clears all existing screenshots optionally, captures five, then passes. Used by functional tests and visual QA for temporal analysis. |
| `camera_nudge_return`    | [tests/camera/CameraNudgeReturnTest.cpp](../tests/camera/CameraNudgeReturnTest.cpp)               | Screenshots the loaded scene, drags the camera out and back to its starting point through the mouse input path, then screenshots again. Driven by [tests/camera/camera_nudge_test.py](../tests/camera/camera_nudge_test.py), which FLIP-compares the two screenshots. Enforces the forward pipeline. |
| `usd_round_trip`         | [tests/usd/UsdRoundTripTest.cpp](../tests/usd/UsdRoundTripTest.cpp)                               | Renders the loaded scene, exports it to USD, loads the exported file back and renders it again. Driven by [tests/usd/usd_roundtrip_test.py](../tests/usd/usd_roundtrip_test.py), which FLIP-compares the two screenshots. See [USD.md](USD.md). |
| `cooker_request`         | [tests/cook/CookerRequestTest.cpp](../tests/cook/CookerRequestTest.cpp)                           | Verifies logical and relocated-content cache hits share main-thread delivery, avoid source construction on an exact hit, and avoid recooking identical relocated content without exporting cache metadata to the requester. |
| `ibl_parity`             | [tests/cook/IblParityTest.cpp](../tests/cook/IblParityTest.cpp)                                   | Gates the CPU IBL cook jobs against their GPU producers: deletes the internal IBL artifacts, lets the GPU cook them, runs the CPU jobs and compares payloads. Needs a physical GPU; part of the macos suite. See [Cooking.md](Cooking.md). |
| `scene_load_failure`     | [tests/scene/SceneLoadFailureTest.cpp](../tests/scene/SceneLoadFailureTest.cpp)                   | Verifies a missing authored sky resource preserves its path, produces no invalid cube, reports scene async failure, and finishes render-side application before the scene settles. |
| `render_target_pool`     | [tests/rhi/RenderTargetPoolTest.cpp](../tests/rhi/RenderTargetPoolTest.cpp)                       | Exercises `RHIRenderTargetPool` on the render thread: distinct targets while held, reuse of a freed target after the GPU safety delay, manual release of free targets via `ReleaseUnused`. |
| `pipeline_switch_pool`   | [tests/rhi/PipelineSwitchPoolTest.cpp](../tests/rhi/PipelineSwitchPoolTest.cpp)                   | Enforces the forward pipeline, switches at runtime to gpu (or deferred without hardware ray tracing) and back, and asserts the returning forward renderer reuses a pooled render target. |
| `nrd_probe`              | [tests/nrd/NrdProbeTest.mm](../tests/nrd/NrdProbeTest.mm)                                         | Compiles every cooked ReBLUR pipeline to a PSO through the production cooked-shader loader and `MetalNrdBackend`, proving the GPU supports the SIMD/quad-group ops ReBLUR uses. Apple platforms only. See [Nrd.md](Nrd.md). |
| `denoiser_sweep`         | [tests/nrd/DenoiserSweepTest.cpp](../tests/nrd/DenoiserSweepTest.cpp)                             | Captures consecutive frames under a configurable camera sweep (yaw / pitch / static) for temporal analysis; driven by [tests/nrd/run_nrd_gates.py](../tests/nrd/run_nrd_gates.py). See [Nrd.md](Nrd.md). |
| `nrd_runtime_toggle`     | [tests/nrd/NrdRuntimeToggleTest.cpp](../tests/nrd/NrdRuntimeToggleTest.cpp)                       | Guards enabling NRD at runtime: one arm switches pipeline and enables NRD mid-accumulation (recycled-memory repro), the other enables it on an already-converged frame. |
