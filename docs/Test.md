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

## Test Registry

[tests/registry.json](../tests/registry.json) lists the runnable suite cases. Versatile test cases (e.g. `screenshot`) never appear here bare; each entry pins one to specific arguments (e.g. `forward_render_static` is `screenshot` with `--pipeline forward` plus its ground-truth evaluator), so a case name always identifies one exact, reproducible run.

A case carries:

* `test_case`: the C++ `TestCase` it runs.
* `app_args` / `scene_args`: cvar arguments for the app run; `scene_args` apply only when the suite is given `--scene`. `{framework}`, `{scene}` and `{scene_stem}` expand at run time.
* `evaluator`: an optional Python script (with its own `args` / `scene_args`) that judges the app output afterwards.
* `recooks`: marks a case whose deliberate runtime cook is incompatible with `--require_cooked`; the suite drops it under that flag.

## Test Coverage

[tests/coverage.json](../tests/coverage.json) assigns each CI triplet (`host-framework-config`, e.g. `macos-macos-release`) the registry cases it must run, in execution order. This file decides which triplets CI tests: [dev/ci_matrix.py](../dev/ci_matrix.py) derives the CI test matrix from it, so covering a new triplet means adding its case picks here and its suite invocation to `TEST_RUNNERS` in `ci_matrix.py`. A triplet absent from the file ships untested — currently only `macos-macos-release`, `macos-glfw-release` and `windows-glfw-release` have capable runners. Registry cases picked by no triplet are development-only and run via `--case`.

Unit tests under `tests/build_system/` enforce consistency: unique registry names, test cases that resolve to real `TestCaseRegistrar` registrations, existing evaluators, and coverage that picks existing registry cases for triplets `ci_matrix.py` can run.

## Test Orchestration

`dev/run_tests.py` runs the Python unit tests as a fail-fast preflight, builds once, then derives the current triplet from the host OS and the `--framework` / `--config` arguments and runs the cases [tests/coverage.json](../tests/coverage.json) assigns to it. The application suite runs to completion and reports all failures together.

```bash
python3 dev/run_tests.py --framework macos --config Release --headless
python3 dev/run_tests.py --framework glfw --config Release --software --headless   # on Windows
```

Use `--case <name>` (repeatable) to run registry cases by name regardless of coverage — e.g. `--case forward_render_static` to focus one pipeline, or `--case gpu_render_static` for a development-only case. Use `--skip_build` only when the intended binary is already built. CI runs the packaged Windows build under Lavapipe with `--software --require_cooked`, and the packaged macOS builds (both the macos framework and the glfw/Vulkan one via MoltenVK) on a physical Metal GPU with `--require_cooked`, which rejects runtime cooking. See [CI.md](CI.md) for the exact commands and why the gpu pipeline stays local-only.

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
5. If the suite should run it, add a [tests/registry.json](../tests/registry.json) entry with the exact arguments and pick it in [tests/coverage.json](../tests/coverage.json) for the triplets that must run it. Development-only cases need no coverage pick; without a registry entry either, they still run directly via `--test_case`.

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

Test cases self-register by name: search `tests/` for `TestCaseRegistrar` to enumerate them. The suite-runnable ones live in [tests/registry.json](../tests/registry.json) with their CI assignment in [tests/coverage.json](../tests/coverage.json) (see [Test Registry](#test-registry)); purely development-facing cases (e.g. `multi_frame_screenshot`, `denoiser_sweep`) are documented with the feature they serve.
