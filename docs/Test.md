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
* `recooks`: marks a case whose deliberate runtime cook would trip the suite's cook gate; coverage runs drop it, so it runs only via `--case`.

## Test Coverage

[tests/coverage.csv](../tests/coverage.csv) is the coverage table: one row per registry case, one column per CI triplet (`host-framework-config`, e.g. `macos-macos-release`), and an `x` wherever a triplet must run a case. Row order is the suite execution order. The table decides which triplets CI tests: [dev/ci_matrix.py](../dev/ci_matrix.py) derives the CI test matrix from its columns. A triplet without a column ships untested — currently `macos-macos-release`, `macos-glfw-release`, `windows-glfw-release`, `ubuntu-glfw-release` and `ubuntu-android-release` (an emulator, see [Android](#android)) have capable runners. Registry cases without a row (or with an empty row) are development-only, run via `--case`.

Maintaining the two tables:

* Adding a test case: append a [tests/registry.json](../tests/registry.json) entry (unique `name`, the C++ `test_case` it pins, `app_args`, optional evaluator) and a [tests/coverage.csv](../tests/coverage.csv) row at its execution-order position, marking every triplet that must run it. Development-only cases need no row; every row must name a registry case.
* Adding a platform: add a triplet column to [tests/coverage.csv](../tests/coverage.csv), mark its picks, and give the triplet a suite invocation in `TEST_RUNNERS` in [dev/ci_matrix.py](../dev/ci_matrix.py).
* Retiring a case or platform: remove both sides (registry entry and coverage row, or column and `TEST_RUNNERS` entry).

Unit tests under `tests/build_system/` enforce consistency: unique registry names, test cases that resolve to real `TestCaseRegistrar` registrations, existing evaluators, coverage rows picking only registry cases, and a `TEST_RUNNERS` entry for every covered triplet.

## Test Orchestration

`dev/run_tests.py` runs the Python unit tests as a fail-fast preflight, derives the current triplet from the host OS and the `--framework` / `--config` arguments, and runs the cases [tests/coverage.csv](../tests/coverage.csv) assigns to it. The application suite runs to completion and reports all failures together.

```bash
python3 dev/run_tests.py --framework macos --config Release
python3 dev/run_tests.py --framework glfw --config Release --software   # on Windows
```

The suite never builds: it tests the existing build (produce one first, e.g. `python3 build.py --framework macos --config Release`). Every case runs headless — a case that needs a window overrides that itself through `OnEnforceConfigs()` — and coverage runs are cook-gated: any on-the-fly cook fails the run. Use `--case <name>` (repeatable) to run registry cases by name regardless of coverage — e.g. `--case forward_render_static` to focus one pipeline, or `--case gpu_render_static` for a development-only case; `--case` runs skip the cook gate, which is how deliberately recooking cases like `ibl_parity` run. CI runs the packaged Windows and Linux builds under Lavapipe with `--software`, and the packaged macOS builds (both the macos framework and the glfw/Vulkan one via MoltenVK) on a physical Metal GPU. See [CI.md](CI.md) for the exact commands and why the gpu pipeline stays local-only.

The static-render evaluator lives at [tests/screenshot/static_render_test.py](../tests/screenshot/static_render_test.py), and the USD evaluator lives at [tests/usd/usd_roundtrip_test.py](../tests/usd/usd_roundtrip_test.py). They own specialized assertions; they do not orchestrate the general suite. Shared framework, dependency and image mechanics live in [tests/rendering/render_test_support.py](../tests/rendering/render_test_support.py), so one evaluator never serves as another evaluator's utility layer.

## Android

The suite runs on a device or emulator through the android runner in [build_system/android/build.py](../build_system/android/build.py). Android has no argv or exit-code channel, so the runner delivers cvars as the runtime config file (`<external-storage-path>/config/config.json`, see [Run.md](Run.md)), starts the activity, and reads the verdict from the `Test case '<name>' passed/failed` logcat markers. After each case it pulls the app log and screenshots to `build_system/android/output/device/`, where the suite's cook gate and the Python evaluators expect them.

* If no device is connected, the runner boots the `sparkle_test` AVD headless (arm64, like every shipped apk), installing the system image and creating the AVD on first use. It needs the SDK `cmdline-tools` package (`sdkmanager`/`avdmanager`).
* First use also seeds a quickboot snapshot (cold boot, settle, clean shutdown); every later boot resumes from it in seconds. CI caches the AVD directory between runs, so only cache-miss runs pay the cold boot.
* The config push requires `adb root` (available on emulator `google_apis` images): files a non-root shell creates under `Android/data` are invisible to the app through FUSE. On a non-rootable physical device the push may still work depending on the vendor's FUSE behavior.
* A `TestCase` can ask the runner for device-level actions by logging `[TestCaseAction] <name>`; `surface_loss_recovery` uses the `cycle_window` action (screen off/on) to destroy and restore the native window mid-run.
* Render cases compare against the published android ground truth, which is captured at 1560x720; pass `--width 1560 --height 720` so headless screenshots match.
* CI covers the `ubuntu-android-release` triplet with a dedicated x86_64 package (`--android_abi x86_64`, a product that exists only for this cell) on a KVM-accelerated emulator, because no hosted runner can emulate the shipping arm64 apk: Google publishes no linux-arm64 emulator, x86 hosts reject arm64 system images, and GitHub's Apple-silicon runners expose no nested virtualization. The emulator image ABI follows the host, so local runs on Apple silicon test the arm64 apk itself.

```bash
# package once, then run cases on a local emulator (boots one if no device is attached)
python3 build.py --framework android --config Release
python3 dev/run_tests.py --framework android --config Release --case smoke
```

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
python3 run.py --framework [glfw, macos] --test_case smoke --headless true
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
5. If the suite should run it, add a [tests/registry.json](../tests/registry.json) entry with the exact arguments and mark its [tests/coverage.csv](../tests/coverage.csv) row for the triplets that must run it (see [Test Coverage](#test-coverage) for how to maintain both tables). Cases without a registry entry still run directly via `--test_case`.

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
python3 run.py --framework macos --test_case my_feature
```

### Naming Rules

Test case names (the string passed to `TestCaseRegistrar`) must be unique across all `.cpp` files under `tests/`. Duplicate names are detected at startup and logged as an error; the first registration wins.

## Built-in Test Cases

Test cases self-register by name: search `tests/` for `TestCaseRegistrar` to enumerate them. The suite-runnable ones live in [tests/registry.json](../tests/registry.json) with their CI assignment in [tests/coverage.csv](../tests/coverage.csv) (see [Test Registry](#test-registry)); purely development-facing cases (e.g. `multi_frame_screenshot`, `denoiser_sweep`) are documented with the feature they serve.
