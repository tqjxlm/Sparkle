# CI/CD Guide

## General Info

* A CI pipeline is setup in github [actions](https://github.com/tqjxlm/Sparkle/actions) at .github/workflows/ci.yml
* Pushing tags will generate releases on github. The release process is defined in .github/workflows/release.yml.
* All PRs are required to pass CI before merging.
* The required gates are formatting, clang-tidy, the platform build matrix, cooking and package assembly, and the aggregate test suite. Performance testing is not yet automated.

## Pipeline Graph

[.github/workflows/build.yml](../.github/workflows/build.yml) runs four stages:

* **build**: every product (framework × config) in parallel; builds are the heavy nodes and none of them waits for anything.
* **cook**: one macos-release node cooks the shared content on the runner's Metal GPU and publishes the assembled content image as the `cooked-shared` artifact (see [Cooking.md](Cooking.md)).
* **release**: every product: replaces each build product's packed content with the image and re-signs where the rewrite breaks the signature (apk: zipalign + apksigner with the debug key; ios: re-codesign; macos: sign-and-notarize).
* **test**: a test table decides which released products run the aggregate suite and with what coverage. Currently enabled: windows-release under lavapipe, and macos-release on the runner's physical Metal GPU. Both run with `--require_cooked`. A product absent from the table ships untested — no runner can drive it yet.

All three matrices derive from one product table: a cheap plan node runs [dev/ci_matrix.py](../dev/ci_matrix.py) — which owns the product list, the standalone-build carve-out and the test table — and the matrix jobs consume its JSON through `fromJSON`, so no combination is ever listed twice.

A cell waits only for the shared cook and its own product's upstream cell, never other products'. The release and test matrices carry a real `needs` edge to cook; the edge to the product's own build (for a release cell) or release (for a test cell) cannot be a `needs` (GitHub cannot target a single matrix cell), so it is an await-by-name through the run's job list — the shared [wait-for-job](../.github/actions/wait-for-job/action.yml) action. The awaits start only after cook — by then the cook's own macos build is done, so waiting cells cannot starve the macos build queue. The one standalone job is the macos-release build, which cook's `needs` edge must target.

## Local Validation Gates

Run the same validation classes locally in fail-fast order. The cheap, deterministic checks go first so they reject a change before a build or render test spends time:

```bash
python3 dev/check_format.py
python3 build.py --framework glfw --config Release --clangd
python3 dev/check_tidy.py
python3 dev/run_tests.py --framework macos --config Release --headless
```

On Windows, use the GLFW suite with software Vulkan when no physical GPU is available:

```bash
python3 dev/run_tests.py --framework glfw --config Release --software --headless
```

Use `dev/check_format.py --fix` while editing, but rerun the check-only command for the gate. `--skip_build` is valid only when the intended binary was already built; it must not be used to bypass build validation. A focused test is useful during development but does not replace this sequence when declaring a change complete.

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

`dev/run_tests.py` is the single general test orchestrator; [Test.md](Test.md) documents the suite contents, the TestCase system and focused-test commands. CI runs the suite against two released packages, both in Release mode, headless, and with `--require_cooked`, which fails if a test cooks an asset at runtime instead of using the package's cooked content (`ibl parity` is excluded under that flag: it recooks by design and is deliberately not a CI gate, see [Cooking.md](Cooking.md)).

The Windows + GLFW package runs under [Mesa Lavapipe](https://github.com/pal1000/mesa-dist-win):

```bash
python3 dev/run_tests.py --framework glfw --config Release --skip_build \
  --software --headless --require_cooked
```

The macOS package runs the forward and deferred pipelines on the runner's physical Metal GPU:

```bash
python3 dev/run_tests.py --framework macos --config Release --skip_build \
  --headless --require_cooked
```

The hosted macos runners are VMs whose paravirtualized Metal device reports `supportsRaytracing == false`, so the gpu path-tracing pipeline silently falls back to forward rendering there — its screenshot gate and the NRD gate suite (see [Nrd.md](Nrd.md)) would be vacuous and stay local-only. Enabling them is a test-table change away if a runner with ray tracing (e.g. self-hosted) ever appears.

## Screenshot Ground Truth

The suite compares auto-generated screenshots with the published ground truth. The default run covers forward and deferred; pass `--pipeline forward`, for example, to focus one pipeline during development. `TestScene` is the packaged default scene (`resources/packed/TestScene.usda`, see [USD.md](USD.md)) and is loaded when no `--scene` override is present. Ground-truth images are updated manually.

### TestScene

| framework | cpu                                                                                             | gpu                                                                                               | forward                                                                                               | deferred                                                                                               |
| --------- | ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| glfw      | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_cpu_glfw.png)  | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_glfw.png)    | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_glfw.png)    | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_glfw.png)    |
| macos     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_cpu_macos.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_macos.png)   | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_macos.png)   | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_macos.png)   |
| ios       | too slow to run                                                                                 | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_ios.png)     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_ios.png)     | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_ios.png)     |
| android   | too slow to run                                                                                 | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_android.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_forward_android.png) | [external](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_deferred_android.png) |
