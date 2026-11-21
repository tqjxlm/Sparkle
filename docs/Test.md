# Testing

Tests run inside the real app. A C++ test case drives the running app and returns a verdict; Python evaluators judge its output afterwards; one orchestrator runs the suite on every platform, locally and in CI.

```text
TestCase (tests/**/*.cpp)      C++ class running in the app after the scene loads; its verdict is the exit code
tests/registry.json            pins a TestCase to exact arguments and an optional evaluator: one suite case
tests/coverage.csv             which CI triplet runs which suite case
dev/run_tests.py               runs the current triplet's cases (or named ones), headless and cook-gated
evaluators (tests/<feature>/)  Python scripts judging logs and screenshots after a case
```

## Running Tests

The suite tests an existing build and never builds:

```bash
python3 build.py --framework macos --config Release
python3 dev/run_tests.py --framework macos --config Release
python3 dev/run_tests.py --framework glfw --config Release --software   # Mesa Lavapipe, no GPU needed
```

* `run_tests.py` first runs the Python unit tests as a fail-fast preflight, then derives the triplet (`host-framework-config`) from the host and its arguments and runs the cases [tests/coverage.csv](../tests/coverage.csv) assigns to it. It runs every case and reports all failures together.
* Every case runs headless. A coverage run is cook-gated: a case that cooks an asset at runtime instead of using cooked content fails.
* `--case <name>` (repeatable) runs registry cases by name regardless of coverage and without the cook gate, e.g. `--case forward_render_static`.
* `--scene <path>` runs the cases on another scene; its file name selects the ground truth.
* `--shard K/N` keeps every N-th selected case, starting at the K-th, so N parallel runs split the suite; CI uses it for its longest column.

A single test case also runs directly in the app, without the registry:

```bash
python3 run.py --framework macos --headless true --test_case smoke [--test_timeout <frames>]
```

The exit code is 0 when the case passes, and 1 when it fails or is not registered. `--test_timeout` fails a case that has not finished within that many frames (default 0, no limit). Other cvars work alongside as usual.

## Writing Tests

Write a test case that exercises the new feature or reproduces the bug, or improve an existing one.

1. Create a `.cpp` file under `tests/`, next to the feature it covers.
2. Subclass `sparkle::TestCase` and implement `OnTick()`, which runs every frame and returns `Pending`, `Pass` or `Fail`.
3. Optionally override `OnEnforceConfigs()` to fix config values before render and RHI init, e.g. to force a window in a headless run.
4. Register it with `TestCaseRegistrar<T>` under a name unique across `tests/`; a duplicate is logged at startup and the first registration wins. `TestCase::GetName()` returns it, so logs need not repeat it.
5. If the suite should run it, add a registry entry and a coverage row (see [Registry and Coverage](#registry-and-coverage)).

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
        if (frame_ < 10)
        {
            return Result::Pending;
        }
        return Result::Pass;
    }
};

static TestCaseRegistrar<MyFeatureTest> my_feature_test_registrar("my_feature");
} // namespace sparkle
```

Prefer Python for anything that can be judged from outside the app:

* An evaluator lives under `tests/` next to the feature it judges and owns only its assertions, e.g. [tests/screenshot/static_render_test.py](../tests/screenshot/static_render_test.py) or [tests/rendering/effective_pipeline_test.py](../tests/rendering/effective_pipeline_test.py), which checks the log for the pipeline that actually ran. Shared framework, dependency and image helpers live in [tests/rendering/render_test_support.py](../tests/rendering/render_test_support.py); no evaluator serves as another's utility layer.
* `dev/run_tests.py` is the only script that builds or sequences the general suite.
* Python test dependencies go in [tests/requirements.txt](../tests/requirements.txt).

TestCase support is compiled in by default (`ENABLE_TEST_CASES=1`); `build.py --strip_test` removes it from production builds.

## Registry and Coverage

[tests/registry.json](../tests/registry.json) lists the suite cases. A versatile test case such as `screenshot` never appears bare: each entry pins it to arguments (e.g. `forward_render_static` is `screenshot` with `--pipeline forward` plus its ground-truth evaluator), so a case name identifies one reproducible run. An entry carries:

* `test_case`: the C++ `TestCase` it runs.
* `app_args` / `scene_args`: app arguments; `scene_args` apply only when the suite gets `--scene`. `{name}`, `{framework}`, `{scene}` and `{scene_stem}` expand at run time, also in evaluator arguments.
* `evaluator`: an optional Python script, with its own `args` / `scene_args`.
* `recooks`: the case cooks at runtime on purpose, so coverage runs skip it and it runs only with `--case`.

[tests/coverage.csv](../tests/coverage.csv) has one row per registry case, in execution order, and one column per CI triplet, with an `x` where that triplet runs the case. [dev/ci_matrix.py](../dev/ci_matrix.py) generates one CI test job per column, or several parallel shards of it (see [CI.md](CI.md#pipeline)). Registry cases without a row are development-only. Unit tests under `tests/build_system/` keep the two tables consistent with each other, with the `TestCaseRegistrar` names, the evaluators and the CI runners.

What each triplet adds when choosing columns for a case:

* `windows-glfw-release`, `ubuntu-glfw-release` and `ubuntu-android-release` all render with Mesa lavapipe (the android one inside an x86_64 emulator), so a GPU or driver case learns nothing from a second copy. What is unique to each is the host runtime: the MSVC binary and Win32 paths, the Linux binary, the Android lifecycle, assets and surface. `ubuntu-glfw-release` is the cheapest per case and carries the shared software-rendering load.
* `macos-macos-release` (Metal) and `macos-glfw-release` (Vulkan through MoltenVK) are the only physical GPUs, and with `macos-ios-release` the only ARM triplets. Arch-sensitive code needs one of them plus an x86 column, e.g. astcenc builds NEON on ARM and SSE4.1 elsewhere, so `texture_compression` runs on both sides.
* Every case has a fixed cost before its body runs: about 24 s on the android emulator, 14 s on windows, 1 s on `macos-macos-release`.
* A column skips a case that another of its cases contains. Screenshot cases compare against the same per-pipeline ground truth, so `forward_resize` and `cpu_resize_64spp` end on the frame `forward_render_static` and `cpu_render_static_64spp` check, and `forward_render_scale_change` on the one `forward_render_static_halfres` checks; a `*_sync_validation` case that renders a pipeline's default frame covers that pipeline.

To add a case, append its registry entry and a coverage row at its execution position. To add a platform, add a coverage column, give the triplet a suite invocation in `TEST_RUNNERS` in [dev/ci_matrix.py](../dev/ci_matrix.py) and regenerate CI with `python3 dev/ci_matrix.py --fix`. Retiring either removes both sides.

## Ground Truth

Screenshot cases compare a headless 1280x720 capture with one published image per scene and pipeline, shared by every platform (`FLIP_THRESHOLD` in [tests/screenshot/static_render_reference.py](../tests/screenshot/static_render_reference.py)). The reference scene is **TestScene**, the packaged default scene that loads when `--scene` is empty: a floor, spheres, glTF models and a sky map ([resources/packed/TestScene.usda](../resources/packed/TestScene.usda), produced as described in [USD.md](USD.md#packaged-testscene)).

| pipeline | ground truth                                                                                   |
| -------- | ---------------------------------------------------------------------------------------------- |
| cpu      | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_cpu.png)      |
| gpu      | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu.png)      |
| forward  | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward.png)  |
| deferred | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred.png) |

* `--test_case screenshot` writes `screenshots/screenshot.png` under the [external storage path](Run.md#storage) once the scene is loaded and a frame is complete; `multi_frame_screenshot` writes five consecutive frames as `multi_frame_<N>.png`. The UI's Save Screenshot button names files by scene, pipeline and time.
* The path tracer cases (`*_64spp`) stop at 64 samples per pixel so lavapipe can run them in CI, with a threshold that allows for the remaining noise; the converged `cpu_render_static` and `gpu_render_static` are development-only.
* The maintainer publishes ground truth by uploading `TestScene_<pipeline>.png` captures. A change meant to alter the image fails these cases until then.

## Validation Layer

Once the scene has loaded, every error the Vulkan validation layer reports fails the running test case; warnings are only logged. The layer is active when `validation` is on (the packaged config turns it on) and the loader finds `VK_LAYER_KHRONOS_validation`; otherwise validation is silently off. Errors after the verdict, e.g. objects leaked at device destruction, do not change it. A test that provokes errors on purpose accepts them through `TestCase::AcceptValidationErrors`.

`validate_sync` adds synchronization validation, which reports GPU accesses no barrier orders, also across submits. It is slow, so only the registry cases named `*_sync_validation` turn it on, and CI runs one per pipeline and per render graph feature: the merge parity variants cover the forward and deferred frames, `gpu_primitive_move_sync_validation` the gpu one. `sync_validation_hazard` records an unsynchronized write to prove the gate can fail. A run with `validate_sync true` fails unless synchronization validation is actually active, so the gate never passes vacuously. Run them locally with `--case` on a glfw build whose loader finds the layer.

## Platform Runners

### Android

The runner in [build_system/android/build.py](../build_system/android/build.py) drives a device or emulator. Android has no argv or exit code, so the runner writes cvars to the user config, starts the activity and reads the verdict from the `Test case '<name>' passed/failed` logcat markers. After each case it pulls the log and screenshots to `build_system/android/output/device/`, where the cook gate and the evaluators read them.

```bash
python3 build.py --framework android --config Release
python3 dev/run_tests.py --framework android --config Release --case smoke   # boots an emulator if no device is attached
```

* Without a device, the runner boots the `sparkle_test` AVD headless, creating it and its system image on first use (needs the SDK `cmdline-tools`). A quickboot snapshot seeded on first use makes later boots take seconds.
* Pushing the config needs `adb root`, available on emulator `google_apis` images, because files a non-root shell creates under `Android/data` are invisible to the app.
* A case requests device actions by logging `[TestCaseAction] <name>`: `cycle_window` (screen off and on) destroys and restores the native window, `resize_display` changes the aspect ratio. The display size resets after every attempt.
* The device runs the app, so a coverage run executes the `ubuntu-android-release` column from any host. The emulator ABI follows the host: Apple silicon tests the arm64 apk, CI's x86 runners an `--android_abi x86_64` package.

### iOS

The runner in [build_system/ios/build.py](../build_system/ios/build.py) runs a `--ios_platform simulator` package inside the iOS Simulator. The app binary runs as a plain headless process through `simctl spawn`, so cvars pass as arguments and the exit code is the verdict.

```bash
python3 build.py --framework ios --config Release --ios_platform simulator
python3 dev/run_tests.py --framework ios --config Release --case smoke
```

* The runner boots the `sparkle_test` simulator, created from the newest installed iPhone type; runs are headless, so the model does not matter.
* After each case it copies the log and screenshots to `build_system/ios/output/device/`.
* The simulator reports no ray tracing, so the gpu pipeline stays local-only, and rejects shared-storage textures, so the Metal backend keeps textures private there (see `MetalImage`).

### Windowed Cases

Suite runs are headless, so cases that need a window have no registry entry: `windowed_flush` (frames keep presenting while the main thread flushes the render thread) and `render_graph_ui` (the UI merges into the screen pass). Run them without `--headless`, e.g. `python3 run.py --framework macos --test_case windowed_flush`.
