# CI/CD Guide

## General Info

* A CI pipeline is setup in github [actions](https://github.com/tqjxlm/Sparkle/actions) at .github/workflows/ci.yml — a fully generated file, never edited by hand (see [Pipeline Graph](#pipeline-graph)).
* Pushing a version tag runs the same pipeline and additionally assembles a github release from the shipped packages (the `github-release` job).
* All PRs are required to pass CI before merging.
* The required gates are formatting, clang-tidy, the platform build matrix, cooking and package assembly, and the aggregate test suite. Performance testing is not yet automated.

## Pipeline Graph

[.github/workflows/ci.yml](../.github/workflows/ci.yml) runs four stages:

* **build**: every product (framework × config) in parallel; builds are the heavy nodes and none of them waits for anything. Debug cells are compile gates only: they ship no release package and therefore run no release or test node.
* **cook**: one macos-release node cooks the shared content on the runner's Metal GPU and publishes the assembled content image as the `cooked-shared` artifact (see [Cooking.md](Cooking.md)).
* **release**: every released product: replaces each build product's packed content with the image and re-signs where the rewrite breaks the signature (apk: zipalign + apksigner with the debug key; ios: re-codesign; macos: sign-and-notarize).
* **test**: the coverage table ([tests/coverage.csv](../tests/coverage.csv)) decides which released products run the aggregate suite and which registry cases they run; each of its columns becomes a test job. Currently enabled: windows-glfw-release and ubuntu-glfw-release under lavapipe, macos-macos-release on the runner's physical Metal GPU, macos-glfw-release exercising the Vulkan backend on that GPU through MoltenVK, macos-ios-release inside the iOS Simulator (a dedicated unsigned simulator package), and ubuntu-android-release on a KVM-accelerated emulator (a dedicated x86_64 package; see [Test.md](Test.md)). A product without a column ships untested — no runner can drive it yet. How to maintain the registry and coverage tables is documented in [Test.md](Test.md); the CI-side half of a new triplet is its `TEST_RUNNERS` suite invocation in [dev/ci_matrix.py](../dev/ci_matrix.py).

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

The hosted macos runners are VMs whose paravirtualized Metal device reports `supportsRaytracing == false`, so the gpu path-tracing pipeline silently falls back to forward rendering there — its screenshot gate (`gpu_render_static`) and the NRD gate suite (see [Nrd.md](Nrd.md)) would be vacuous and stay local-only. Enabling them is a coverage-file change away if a runner with ray tracing (e.g. self-hosted) ever appears.

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
