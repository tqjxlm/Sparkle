# CI/CD Guide

## General Info

* A CI pipeline is setup in github [actions](https://github.com/tqjxlm/Sparkle/actions) at .github/workflows/ci.yml — a fully generated file, never edited by hand (see [Pipeline Graph](#pipeline-graph)).
* Pushing a version tag runs the same pipeline and additionally assembles a github release from the shipped packages (the `github-release` job).
* All PRs are required to pass CI before merging.
* The required gates are formatting, clang-tidy, the platform build matrix, cooking and package assembly, and the aggregate test suite. Performance testing is not yet automated.

## Pipeline Graph

[.github/workflows/ci.yml](../.github/workflows/ci.yml) runs four stages:

* **build**: every product (framework × config) in parallel; builds are the heavy nodes and none of them waits for anything. Debug cells are compile gates only: they ship no release package and therefore run no release or test node, and a product whose Debug gate is already covered by another cell builds Release only (`build_types` in the `PRODUCTS` table of [dev/ci_matrix.py](../dev/ci_matrix.py)).
* **cook**: two nodes, split by texture family. The macos-release node cooks every fp16 master on the runner's Metal GPU plus the ASTC targets, and publishes the artifact pool alongside them; the ubuntu node seeds that pool, so every master is a cache hit and the BC targets need no GPU. Each target's content image is its own `cooked-image-<target>` artifact, and a release node waits only on its own family's cook (see [Cooking.md](Cooking.md)).
* **release**: every released product: replaces each build product's packed content with its own target's image and re-signs where the rewrite breaks the signature (apk: zipalign + apksigner with the debug key; ios: re-codesign; macos: sign-and-notarize).
* **test**: the coverage table ([tests/coverage.csv](../tests/coverage.csv)) decides which released products run the aggregate suite and which registry cases they run; each of its columns becomes a test job. Currently enabled: windows-glfw-release under lavapipe, ubuntu-glfw-release (arm64) on a self-hosted Jetson, the only cell with hardware ray tracing, macos-macos-release on the runner's physical Metal GPU, macos-glfw-release exercising the Vulkan backend on that GPU through MoltenVK, macos-ios-release inside the iOS Simulator (a dedicated unsigned simulator package), and ubuntu-android-release on a KVM-accelerated emulator (a dedicated x86_64 package; see [Test.md](Test.md)). A product without a column ships untested — no runner can drive it yet. How to maintain the registry and coverage tables is documented in [Test.md](Test.md); the CI-side half of a new triplet is its `TEST_RUNNERS` suite invocation in [dev/ci_matrix.py](../dev/ci_matrix.py).

ci.yml is a fully generated file: GitHub's `needs` cannot target a single matrix cell, so a runtime matrix cannot express the per-product edges (release → its own build, test → its own release) without runners idling in polling loops. [dev/ci_matrix.py](../dev/ci_matrix.py) owns the product table and the per-triplet suite invocations, and unrolls them into explicit jobs: every edge is a real `needs`, no runner is ever requested before its dependencies are done, and every node renders flat as `stage (os, framework, config[, abi])`. Never edit ci.yml by hand — change the generator, then regenerate with `python3 dev/ci_matrix.py --fix`. Two gates enforce byte-exact freshness: a pre-commit hook (`.githooks/`, wired automatically by any `build.py` run) rejects the commit, and the format job fails the push.

## Local Validation Gates

Run the same validation classes locally in fail-fast order. The cheap, deterministic checks go first so they reject a change before a build or render test spends time:

```bash
python3 dev/check_format.py
python3 dev/ci_matrix.py
python3 build.py --framework glfw --config Release --clangd
python3 dev/check_tidy.py
python3 dev/run_tests.py --framework macos --config Release
```

On Windows or Linux, use the GLFW suite with software Vulkan when no physical GPU is available:

```bash
python3 dev/run_tests.py --framework glfw --config Release --software
```

Use `dev/check_format.py --fix` while editing, but rerun the check-only command for the gate. The suite tests an existing build, so build first; a focused test is useful during development but does not replace this sequence when declaring a change complete.

## Format Check

* Format check is run on every push and PR, regardless of which files changed.
* It verifies that all tracked and untracked, non-ignored source files (thirdparty excluded) match the output of the project formatters:
  * c++/objc/slang: clang-format with [.clang-format](../.clang-format)
  * python: autopep8, configured in [pyproject.toml](../pyproject.toml)
  * markdown: markdownlint with [.markdownlint.json](../.markdownlint.json)
* Run it locally before pushing:

```bash
python3 dev/check_format.py          # check only
python3 dev/check_format.py --fix    # rewrite files in place
```

* Requirements: `pip install clang-format==22.1.5 autopep8` and Node.js (markdownlint runs via npx). clang-format majors 18-22 all produce identical results on this codebase; CI pins 22.1.5.

## Clang-Tidy Check

* Clang-tidy is run on every push and PR that touches code, with [.clang-tidy](../.clang-tidy). All warnings are treated as errors.
* It checks every first-party source file in the glfw compile database, so platform-exclusive code (e.g. metal) is not covered.
* Run it locally before pushing:

```bash
python3 build.py --framework glfw --clangd   # generate the compile database (once)
python3 dev/check_tidy.py                    # check all first-party sources
```

* Requirements: `pip install clang-tidy==22.1.7`. Unlike clang-format, majors are not interchangeable; CI enforces major 22.

## Build Test

* Build test is run on every push and PR. It builds the project with different frameworks and pipelines.
* Compiled objects are cached across CI runs with [ccache](https://ccache.dev), keyed per os/framework/config (see .github/actions/setup-environment). A warm run only recompiles files affected by the change; a cold cache (first run, or after a toolchain update) pays the full build cost once. Each build resets the restored cache's counters before compiling, so the `Compiler cache stats` step reports that job's hit rate rather than cumulative history.
* When judging build speed, compare step durations rather than total job time: a job can sit queued for a long time waiting for a free runner before its first step starts. This especially affects the macOS jobs, which outnumber the concurrent macOS runners GitHub provides and therefore partly serialize.
* Archived builds will be uploaded on successful runs. You can download them from the [actions](https://github.com/tqjxlm/Sparkle/actions) page.
  * NOTICE: only download artifacts from workflow triggered by yourself. Security cannot be guaranteed for artifacts generated by untrusted developers.
  * NOTICE: due to certificate limitation, iOS artifacts cannot be installed on your machine. Please build with your own developer account to test them. Or you can ask me to register your device in my provisioning profile.

## Aggregate Test Suite

`dev/run_tests.py` is the single general test orchestrator; [Test.md](Test.md) documents the suite contents, the TestCase system and focused-test commands. CI runs the suite against every released package the coverage table names, all in Release mode. The suite is always headless and cook-gated: it fails if a test cooks an asset at runtime instead of using the package's cooked content (`ibl_parity` is excluded: it recooks by design and is deliberately not a CI gate, see [Cooking.md](Cooking.md)).

The Windows + GLFW package runs under [Mesa Lavapipe](https://github.com/pal1000/mesa-dist-win):

```bash
python3 dev/run_tests.py --framework glfw --config Release --software
```

The Linux + GLFW package is **arm64** and runs on a self-hosted NVIDIA Jetson, whose Tegra GPU is the only one in the matrix that exposes `VK_KHR_ray_query`. That makes it the only cell that can render the gpu path-tracing pipeline, so it carries `gpu_render_static`; the software-rasterizer coverage this cell used to provide now lives entirely on the Windows one:

```bash
python3 dev/run_tests.py --framework glfw --config Release
```

No hosted runner has a GPU, and hosted macos reports `supportsRaytracing == false` through its paravirtual Metal device, so hardware ray tracing in CI needs hardware someone owns. See [Jetson test runner](#jetson-test-runner) for what the board needs and how its access is bounded.

The whole linux product moved to arm64 rather than adding a second one: the shipped binary has to be the one the test cell runs, and GitHub's arm64 hosted runners build it at no cost on a public repo. The cook stage follows the same architecture, because the BC node encodes by running that product's own binary — the cooked output it produces is architecture-independent, so the cook graph is otherwise unchanged.

The product is **cross-compiled on an x86 host** rather than built natively, because nothing that runs *during* the build ships an aarch64 linux binary: LunarG publishes no aarch64 Vulkan SDK, and DXC — which NRD needs to compile its HLSL to SPIR-V — has no official aarch64 linux release either. Cross-compiling keeps dxc, slangc, ispc and ShaderMake on the host where those prebuilts exist, and only the linked libraries come from the target ([cmake/aarch64-linux-toolchain.cmake](../cmake/aarch64-linux-toolchain.cmake), selected by `build.py --target_arch aarch64`). Target libraries come from multiarch (`libvulkan-dev:arm64`, `libglfw3-dev:arm64` off ports.ubuntu.com), so the runner needs its existing sources pinned to amd64 first.

Three hosts therefore appear for one product, and the split is deliberate:

| stage | host | why |
| ----- | ---- | --- |
| build | `ubuntu-latest` (x86) | the build-time tools only exist for x86 |
| cook | `ubuntu-24.04-arm` | it encodes by *running* the product's own binary, which is arm64 |
| test | the jetson | the only runner exposing `VK_KHR_ray_query` |

A job that executes on arm64 still needs a host Vulkan SDK for `build.py`, and there is none to download, so those runners take `libvulkan-dev` and `spirv-cross` from the distribution and [build_system/prerequisites.py](../build_system/prerequisites.py) adopts `/usr` as `VULKAN_SDK`. cmake, ninja, ispc and slang all publish aarch64 assets, selected by `host_arch()`. The prerequisite cache is keyed by runner architecture, since it holds host binaries and the cook node shares an os label with the x86 build.

### Jetson test runner

The board is a self-hosted GitHub runner registered against this repository with the labels in `JETSON_LABELS` ([dev/ci_matrix.py](../dev/ci_matrix.py)); they must match `./config.sh --labels` on the board.

### Running it on demand

The board is a machine someone switches on, so its suite can be launched instead of waiting for a push. [.github/workflows/jetson-test.yml](../.github/workflows/jetson-test.yml) is a `workflow_dispatch` workflow that runs the test node alone, and [dev/run_jetson_test.py](../dev/run_jetson_test.py) dispatches it from a clone:

```bash
python3 dev/run_jetson_test.py                        # newest successful run's package
python3 dev/run_jetson_test.py --case gpu_render_static --watch
```

The test node consumes the released package rather than building one, so a manual run takes that artifact from a completed CI run — by default the newest successful one for the current branch, or `--run-id` for a specific one. The launcher resolves it locally so the board needs no gh CLI and no credentials of its own; it only ever receives a run id. That workflow is hand-written rather than generated, and a unit test holds it to the same runner, package and suite invocation as the automatic cell.

Leave `JETSON_RUNNER` unset to rely on this alone.

### Running it automatically

The cell is admitted by a repository variable, `JETSON_RUNNER`, which must be set to `true` for it to run at all. This is not decoration: a job whose labels match no runner does not fail, it queues for the 24 hours GitHub allows and only then cancels, taking the aggregate gate down a day late. The variable makes an unregistered or offline board skip the cell instead, and doubles as a kill switch when the board is down.

Because a self-hosted runner executes whatever a workflow tells it to, on hardware in someone's home, the cell is closed to forks:

```yaml
if: github.event_name == 'push' ||
    github.event.pull_request.head.repo.full_name == github.repository
```

Pull requests from a fork skip this cell entirely and lose only its linux coverage; the maintainer's own branches still gate on it pre-merge.

That condition is defence in depth, not the gate. It lives in a generated file that a fork's pull request can rewrite, since a `pull_request` run executes the workflow as it exists on the PR head. The actual gate is **Settings → Actions → Fork pull request workflows from outside collaborators → Require approval for all outside collaborators** — the default only requires approval for *first-time* contributors, so one merged PR is enough to earn unattended runs on someone's hardware. Register the runner at repository scope rather than account scope, treat the labels as addressing rather than authorization, and never reach this runner from a `pull_request_target` workflow, which runs regardless of the approval setting.

The runner dials out to GitHub over HTTPS and needs no inbound network exposure. The board keeps its own driver and runtime libraries (`libglfw3`, an L4T Vulkan driver), which is why this cell installs neither, and the job carries no secrets at all.

The macOS package runs the forward and deferred pipelines on the runner's physical Metal GPU:

```bash
python3 dev/run_tests.py --framework macos --config Release
```

The macOS + GLFW package runs the same Vulkan backend as Windows, but on the runner's physical Metal GPU through MoltenVK:

```bash
python3 dev/run_tests.py --framework glfw --config Release
```

The Android cell runs the dedicated x86_64 package on a KVM-accelerated headless emulator, whose guest Vulkan device is llvmpipe — the same software-rasterizer class as the Windows cell (the shipping arm64 apk cannot be emulated on any hosted runner; see the Android section in [Test.md](Test.md)). Its picks include `surface_loss_recovery`, which exercises the Android native-window teardown/recreate path. `--width 1560 --height 720` matches the resolution of the published android ground-truth captures:

```bash
python3 dev/run_tests.py --framework android --config Release --width 1560 --height 720
```

The iOS cell runs the dedicated unsigned simulator package as spawned headless processes inside the iOS Simulator (the shipping ipa targets physical devices, which no hosted runner can drive; see the iOS section in [Test.md](Test.md)). `--width 1565 --height 720` matches the resolution of the published ios ground-truth captures:

```bash
python3 dev/run_tests.py --framework ios --config Release --width 1565 --height 720
```

The hosted macos runners are VMs whose paravirtualized Metal device reports `supportsRaytracing == false`, so the gpu path-tracing pipeline silently falls back to forward rendering there. `gpu_render_static` therefore runs on the Jetson linux cell and nowhere else: on any other cell it would render a forward frame, compare it against the path-traced ground truth, and report whatever that comparison happened to yield. The Metal-side denoiser gate suite (see [Denoiser.md](Denoiser.md)) has no such home yet and stays local-only, because its backend is Metal — the Jetson reaches NRD through Vulkan and never touches it.

The paravirtual device also renders MTLHeap-placed resources as solid magenta through MoltenVK without reporting any error, so the test job runs with `MVK_CONFIG_USE_MTLHEAP=0` (dedicated allocations). Real GPUs render identically either way; if a macos-glfw cell ever regresses to uniform magenta screenshots, suspect this class of paravirtual quirk first.

## Screenshot Ground Truth

The suite compares auto-generated screenshots with the published ground truth. CI coverage spans forward and deferred; pass `--case forward_render_static`, for example, to focus one pipeline during development. `TestScene` is the packaged default scene (`resources/packed/TestScene.usda`, see [USD.md](USD.md)) and is loaded when no `--scene` override is present. Ground-truth images are updated manually.

### TestScene

| framework | cpu                                                                                             | gpu                                                                                               | forward                                                                                               | deferred                                                                                               |
| --------- | ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| glfw      | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_cpu_glfw.png)  | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_glfw.png)    | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_glfw.png)    | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_glfw.png)    |
| macos     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_cpu_macos.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_macos.png)   | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_macos.png)   | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_macos.png)   |
| ios       | too slow to run                                                                                 | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_ios.png)     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_ios.png)     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_ios.png)     |
| android   | too slow to run                                                                                 | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_android.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_android.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_android.png) |
