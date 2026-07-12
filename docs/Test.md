# Test Guide

## Visual Test

* Use `--test_case screenshot` to take a screenshot after the scene is fully loaded and a frame is fully rendered.
* Use `--test_case multi_frame_screenshot` to take 5 frames of screenshots after the scene is fully loaded and a frame is fully rendered. This is useful for temporal analysis.
* Screenshot test cases work with `--headless true`, so it is suitable for commandline use.
* Screenshots are saved to [external-storage-path]/screenshots/. For `--test_case screenshot`, the file is named `screenshot.png`. For `--test_case multi_frame_screenshot`, files are named `multi_frame_N.png` where `N` is the frame index. The UI "Save Screenshot" button names files with the scene name, pipeline, and timestamp. For [external-storage-path], refer to [Run.md](Run.md).
* Ground truth images can be found in [CI.md](CI.md). But if you are working on a feature that is meant to change the final image output, you should not rely on the ground truth images.

### Confirm you are looking at the right output first

* With **no `--scene`**, the app loads the built-in **TestScene** (a floor, several spheres, and glTF models against a sky map) — the scene the CI ground truth is rendered from. `--scene <path>` loads a model/scene **file path** instead. A name that is not a real path (e.g. `--scene TestScene`) silently logs an error, renders an **empty default-sky scene**, and still exits `0`. Before analysing anything, **verify the expected geometry is actually in the frame** — it is easy to spend a long time analysing a blank or wrong render.
* Likewise confirm the intended pipeline / debug view / cvars actually took effect (check the image, not just the command line).

### Comparing and diffing renders

* Diff against the **ground truth**, not only A/B between two configs. Only the ground truth tells you which side is correct and whether an artifact is feature-specific (e.g. present in `nrd=on` but absent from the raw render).
* Inspect a **per-pixel signed difference** (where one image is brighter vs darker), **zoom to 1:1** on suspect regions, and for edges take **scanline profiles** across the boundary. Thin, high-frequency artifacts — silhouette halos, fringes, ringing, fireflies — are only 1–3 px wide and **average to ≈zero under any whole-image/region mean or low-pass-blurred diff** (an edge band cancels because it straddles the bright overshoot and the adjacent dark side). Never use a blurred or region-averaged diff as the **detector**; use averages only to **quantify** what the eye has already found.
* A passing aggregate metric (FLIP, mean error) does **not** mean the images match — it is a low-pass over the error map and hides spatially concentrated / structural errors. Inspect the worst tiles and the silhouettes explicitly before declaring a match.

## Python Test Scripts

* When possible, always use python scripts to perform tests.
* Make use of any necessary python libraries and add them to dev/requirements.txt
* General test scripts are stored at dev/.
* Dedicated test scripts are stored at tests/.
* If python is not sufficient to test your case, also make use of TestCase system below. They can be combined with python scripts freely. See dev/functional_test.py as an example.

## TestCase System

* A `TestCase` is a C++ class that runs inside the app after the scene finishes loading.
* Before render and RHI config init, the selected test case may override config values via `OnEnforceConfigs()`.
* Each frame, `AppFramework` calls `Tick()`. When `Tick()` returns `Pass` or `Fail` the app
exits with code `0` or `1` respectively.

### Build

TestCase support is compiled in by default (`ENABLE_TEST_CASES=1`).
Use `--strip_test` to exclude it from the binary for production builds:

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

The `--test_case <name>` argument is passed straight through to the app's config system.
Any other cvars (e.g. `--scene`, `--pipeline`) work alongside it as normal.

Use `--test_timeout <frames>` to set a frame budget for the test. If the test does not
finish within the given number of frames it is reported as `Fail`. Default is `0` (no limit).

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

The registered name is injected into the test instance and available via
`TestCase::GetName()`, so log messages do not need to duplicate it manually.

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

Test case names (the string passed to `TestCaseRegistrar`) must be unique across all
`.cpp` files under `tests/`. Duplicate names are detected at startup and logged as an
error; the first registration wins.

## Built-in Test Cases

| Name                     | File                                                                                              | What it does                                                                                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `smoke`                  | [tests/smoke/SmokeTest.cpp](../tests/smoke/SmokeTest.cpp)                                         | Waits 2 frames then returns `Pass`. Verifies the full init and scene-load pipeline.                                                                             |
| `screenshot`             | [tests/screenshot/ScreenshotTest.cpp](../tests/screenshot/ScreenshotTest.cpp)                     | Waits for renderer ready, clears all existing screenshots optionally, captures one, then passes. Used by functional tests and visual QA.                        |
| `multi_frame_screenshot` | [tests/screenshot/MultiFrameScreenshotTest.cpp](../tests/screenshot/MultiFrameScreenshotTest.cpp) | Waits for renderer ready, clears all existing screenshots optionally, captures five, then passes. Used by functional tests and visual QA for temporal analysis. |
| `usd_round_trip`         | [tests/usd/UsdRoundTripTest.cpp](../tests/usd/UsdRoundTripTest.cpp)                               | Renders the loaded scene, exports it to USD, loads the exported file back and renders it again. Driven by [tests/usd/usd_roundtrip_test.py](../tests/usd/usd_roundtrip_test.py), which FLIP-compares the two screenshots. See [USD.md](USD.md). |
| `render_target_pool`     | [tests/rhi/RenderTargetPoolTest.cpp](../tests/rhi/RenderTargetPoolTest.cpp)                       | Exercises `RHIRenderTargetPool` on the render thread: distinct targets while held, reuse of a freed target after the GPU safety delay, manual release of free targets via `ReleaseUnused`. |
| `pipeline_switch_pool`   | [tests/rhi/PipelineSwitchPoolTest.cpp](../tests/rhi/PipelineSwitchPoolTest.cpp)                   | Enforces the forward pipeline, switches at runtime to gpu (or deferred without hardware ray tracing) and back, and asserts the returning forward renderer reuses a pooled render target. |
