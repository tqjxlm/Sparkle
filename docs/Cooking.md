# Cooking

Cooking moves deterministic asset work out of the frame loop: a cook job transforms source data into a validated disk artifact once, and every later run loads the artifact.

## Architecture

* `CookJob` ([libraries/include/core/cook/CookJob.h](../libraries/include/core/cook/CookJob.h)): one deterministic unit of work. `Execute()` runs on a worker thread and must stay CPU-only (no RHI, no scene access), so jobs can run in a render-less process. It returns an explicit `CookJobResult`; failed execution is not represented by an empty payload.
* `CookArtifactStore` ([libraries/include/core/cook/CookArtifactStore.h](../libraries/include/core/cook/CookArtifactStore.h)): owns manifest lookup, artifact validation and persistence across packaged and internal domains. It is independent of scheduling and rendering.
* `Cooker` ([libraries/include/core/cook/Cooker.h](../libraries/include/core/cook/Cooker.h)): orchestrates requests. `Request(job, on_ready)` handles an already-constructed job. `Request(key, job_factory, on_ready)` resolves the logical key first and constructs the source-dependent job on a dedicated thread only after a miss. Cache hits and fresh cooks always return the same `CookHandle` and deliver a `CookResult` on the main thread; destroying the handle cancels delivery, so requesters cannot be called back after death. Concurrent requests for the same lookup key share one execution: the first request runs the job, later ones subscribe to its delivery (enforced by the `cooker_request` test case).
* `SceneCooker` ([libraries/include/scene/cook/SceneCooker.h](../libraries/include/scene/cook/SceneCooker.h)) owns scene loading and build-time execution above the core artifact store. Its caller supplies an explicit `JobPlan`; runtime scene objects expose no build interfaces, and `Cooker` has no static job registry. Scene loading, asynchronous resource resolution, plan collection, execution and store failures all reach the process exit code.
* IBL owns its job declaration in [IblCookPlan](../libraries/include/renderer/resource/IblCookPlan.h) and optional GPU execution in [IblCookAccelerator](../libraries/include/renderer/resource/IblCookAccelerator.h). The application composition boundary connects the resolved scene sky to that plan. GPU passes and the accelerator only generate payloads; they do not know manifests or package policy. Core cooking has no RHI or renderer configuration dependency.

Cooking is additive. Source assets remain ordinary packaged resources, authored references stay private to the implementation that consumes them, and the cooker neither collects raw paths nor controls source stripping.

### Artifacts

An artifact lives at `cooked/<type>/<source stem>_<name hash>.cook` (named by a hash of the normalized logical source path, so same-basename assets do not collide) with a header `{magic, version, source content hash, payload size}`. Each domain carries a manifest (`cooked/manifest.json`, updated on every save) mapping `<type>:<source name>` to the artifact file, version and source content hash — the lookup authority. Lookup order on request:

1. `Path::Resource` — packaged, produced at build time.
2. `Path::Internal` — produced by a previous run of this installation.
3. Miss, version mismatch or invalid header: the job runs and saves to internal storage.

A request whose key has no source hash performs a logical lookup and validates against the hash recorded in the manifest internally. Zero remains a valid resolved hash. If the logical path misses, the worker-side factory loads the source and computes its content hash; the cooker then searches same-type artifacts by version and content before executing the job. This lets relocated but identical assets — such as a sky map copied beside a USD export — reuse an artifact without reducing identity to a collision-prone filename. Cache metadata does not escape through `CookResult` into scene or rendering APIs.

Invalidation is automatic: bump the job's `version` when the algorithm or payload layout changes, and the source content hash covers asset edits where the source exists. The `rebuild_cache` config forces a miss on every lookup.

Saves are atomic: artifact and manifest are written to a temporary file and renamed into place, so a crash or a concurrent reader never observes partial content.

Artifact payloads for full GPU images use the `RHIImage::Upload`/`ReadToMemory` byte order, which is a cross-backend contract: mip-major with array layers inside each mip, rows tightly packed. `VulkanImage` and `MetalImage` must never diverge on this.

## Current cook jobs

| type | source | payload | producers |
| --- | --- | --- | --- |
| `skylight` | equirectangular sky map | 1024 cube (RGBA16F) + extracted sun direction/brightness | CPU |
| `ibl_brdf` | none (constant LUT) | 512x512 RGBA16F | GPU pass, CPU job |
| `ibl_diffuse` | cooked sky cube | 512 cube RGBA16F | GPU pass, CPU job |
| `ibl_specular` | cooked sky cube | 1024 cube RGBA16F, 5 roughness mips | GPU pass, CPU job |

The sky light keeps only its authored path and requests its cook on attach; it does not retain decoded source pixels. It renders a flat sky until delivery. The request is a generation-safe scene async task whose result includes render-side application: failed resolution preserves the authored path, supplies no invalid cube, and makes the scene report resource failure. The IBL jobs derive their identity directly from the cooked cube's pixels, dimensions and format. Scene components, render proxies and renderers therefore do not transport cooker hashes, and any skylight change that affects the actual IBL input invalidates the dependent artifacts automatically. `SkyRenderProxy` owns the IBL resource derived from its sky; renderers consume that resource and never reconstruct its source identity or retain it across a sky-proxy replacement.

At runtime the IBL artifacts are produced by GPU compute passes (frame-budgeted by `ImageBasedLighting::CookOnTheFly`) when a physical GPU is present (`RHIContext::HasPhysicalGpu`), and by CPU jobs otherwise (software rasterizers such as lavapipe). `ImageBasedLighting` owns cache lookup and persistence for runtime generation. In build-time cook mode, `SceneCooker` owns lookup and persistence while `IblCookAccelerator` synchronously returns a GPU-produced payload for supported jobs. Both producers emit the same artifact under the same key.

The IBL alpha channel is producer-dependent (the Metal blit conversion writes 1, the compute path accumulates the sample count) and no consumer reads it.

## CPU/GPU parity

The CPU jobs port the cook shaders one-to-one ([libraries/include/renderer/resource/IblCookMath.h](../libraries/include/renderer/resource/IblCookMath.h) mirrors `shaders/include/{math,random,sampler,cubemap}.h.slang`), including the shader RNG — GPU sample seeds are contiguous per pixel regardless of dispatch batching, so the CPU integrates the identical sample set — plus hardware sampling behavior: the `uv * size - 0.5` texel-center convention and seamless cube edge taps (out-of-face bilinear taps resolve geometrically to the adjacent face).

The `ibl_parity` test case enforces this: it deletes the internal IBL artifacts, lets the GPU cook them, runs the CPU jobs and compares (rgb only, alpha excluded). It is deliberately not a CI gate — the CPU reference cook costs ~15 runner-minutes, and once a CI node cooks through the CPU jobs the screenshot gates cover divergence end to end. It runs as part of the local macos suite (dev/run_tests.py includes it for the macos framework only; a software rasterizer would compare CPU to CPU) or standalone:

```bash
python3 build.py --framework macos --config Release --run --test_case ibl_parity --headless true
```

On failure it writes both payloads to internal storage under `cooked_debug/` for offline analysis. Measured on Apple M-series: `ibl_brdf` max error is one half ULP; the env maps stay under 0.05 against a clamp ceiling of 10.

When editing any `shaders/screen/ibl_*.cs.slang` or the shared shader includes, mirror the change in the CPU port and bump the affected artifact version. Versions live in the job classes ([IblBrdfCookJob.h](../libraries/include/renderer/resource/IblBrdfCookJob.h), [IblEnvCookJobs.h](../libraries/include/renderer/resource/IblEnvCookJobs.h)); shared CPU/GPU algorithm settings live in [IblSettings.h](../libraries/include/renderer/resource/IblSettings.h), not in either producer or a render proxy.

## Release cooking

The bundled cook list, [resources/packed/config/cook_list.json](../resources/packed/config/cook_list.json), declares which scenes contribute derived artifacts to a release cook — currently `TestScene.usda`. The application supplies the cook plan explicitly: the scene-independent BRDF job is always included, and a successfully resolved scene sky contributes the diffuse and specular environment jobs. It does not define the package's raw-resource closure; the normal build packages resources independently.

The tradeoff of additive cooking is package size: every package carries both the source assets and their cooked artifacts — with the current jobs, roughly 130 MB of RGBA16F payloads per sky map (48 MB skylight cube, 67 MB specular chain, 12 MB diffuse, 2 MB BRDF LUT) on top of the source HDR. This is acceptable for the current content set. When content grows, the levers are artifact compression and per-platform compressed texture formats (which introduce the platform key dimension); stripping sources from packages is deliberately not one — dev features such as USD export need them, and cooking stays additive by design.

## CI

The pipeline in [.github/workflows/build.yml](../.github/workflows/build.yml) runs four stages: build, cook, release, test. The build stage covers every platform and runs fully in parallel — builds are the heavy nodes and none of them waits for anything. The cook stage covers only macos-release: it downloads that build's product, runs it headless on the runner's Metal GPU (seconds, unlike software Vulkan), and publishes the cooked content. The release stage packs each build product with the cooked content: [dev/inject_cooked.py](../dev/inject_cooked.py) appends `cooked/` at the platform's resource root inside the archive, and packages whose signature that breaks are re-signed in place (apk: zipalign + apksigner with the debug key; ios: re-codesign; macos: the existing sign-and-notarize action). The test stage currently has one node: it pulls the windows release package and runs the full desktop suite against it, with a log gate failing any run that cooks on the fly. [dev/run_tests.py](../dev/run_tests.py) owns per-platform case selection (and a `--case` filter for subsetting), so adding a tested platform is a new node running the same conductor. A single shared cook serves all platforms because the image payload layout is a cross-backend contract (see Artifacts); genuinely platform-specific content would add per-platform cook nodes.
