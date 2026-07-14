# Cooking

Cooking moves deterministic asset work out of the frame loop: a cook job transforms source data into a validated disk artifact once, and every later run loads the artifact.

Day to day it is automatic. The default `build.py` invocation runs the cook stage after building (see [Build.md](Build.md) for stage selection, host/target rules and packaging), and at runtime a missing artifact cooks on the fly while the requester shows its pending state. Artifacts live in the app's internal storage under `cooked/` (`sparkle.app/Contents/SharedSupport/cooked` for macos, `build/generated/cooked` for glfw); delete that directory or run the app with `--rebuild_cache true` to force a recook. Cook stage runs log to `logs/cook.log` next to the regular app logs.

## Architecture

* `CookJob` ([libraries/include/core/cook/CookJob.h](../libraries/include/core/cook/CookJob.h)): one deterministic unit of work. `Execute()` runs off the main thread and must stay CPU-only (no RHI, no scene access), so jobs can run in a render-less process. It returns an explicit `CookJobResult`.
* `CookArtifactStore` ([libraries/include/core/cook/CookArtifactStore.h](../libraries/include/core/cook/CookArtifactStore.h)): owns manifest lookup, artifact validation and persistence across packaged and internal domains. It is independent of scheduling and rendering.
* `Cooker` ([libraries/include/core/cook/Cooker.h](../libraries/include/core/cook/Cooker.h)): orchestrates requests. `Request(job, on_ready)` handles an already-constructed job. `Request(key, job_factory, on_ready)` resolves the logical key first and constructs the source-dependent job on a dedicated thread only after a miss. Cache hits and fresh cooks always return the same `CookHandle` and deliver a `CookResult` on the main thread; destroying the handle cancels delivery, so requesters cannot be called back after death. Concurrent requests for the same lookup key share one execution. Cache metadata stays inside the cook layer; it does not leak into scene or rendering APIs.
* `SceneCooker` ([libraries/include/scene/cook/SceneCooker.h](../libraries/include/scene/cook/SceneCooker.h)) owns scene loading and build-time execution above the core artifact store. Its caller supplies an explicit `JobPlan`; runtime scene objects expose no build interfaces. Scene loading, asynchronous resource resolution, plan collection, execution and store failures all reach the process exit code.
* IBL owns its job declaration in [IblCookPlan](../libraries/include/renderer/resource/IblCookPlan.h) and optional GPU execution in [IblCookAccelerator](../libraries/include/renderer/resource/IblCookAccelerator.h). The application composition boundary connects the resolved scene sky to that plan. GPU passes and the accelerator only generate payloads; they do not know manifests or package policy. Core cooking has no RHI or renderer configuration dependency.

Cooking is additive: source assets remain ordinary packaged resources, and a package with missing or partial cooked content still works because every job falls back to its raw source and cooks at runtime, per job.

## Adding a cook job

1. Implement `CookJob`: a stable `GetType()` (also the artifact directory name), a `GetVersion()` to bump whenever the algorithm or payload layout changes, `GetSourceName()`/`GetSourceHash()` as the content identity, and a CPU-only `Execute()` (it may fan out through `TaskManager::ParallelFor`).
2. Request it at the natural trigger (attach, resource init) with `Cooker::Request`. Prefer the key + factory overload: a cache hit then never loads or decodes the source. The payload arrives on the main thread.
3. Give the requester a valid pending state — rendering must work before delivery. The sky light renders a flat sky until its cube map arrives.
4. Register the request as a scene async task so screenshot tests and USD export wait for the cook; `SkyLight::RequestCook` is the reference for the whole request-deliver-apply pattern.
5. Wire the job into the application's cook plan (`AppFramework::RunCookMode`) so release cooks produce it, and extend [cook_list.json](../resources/packed/config/cook_list.json) if it depends on a scene not cooked yet.
6. Optional GPU acceleration must produce byte-identical artifacts under the same key — see CPU/GPU parity below.

## Artifacts

An artifact lives at `cooked/<type>/<source stem>_<name hash>.cook` (named by a hash of the normalized logical source path, so same-basename assets do not collide) with a header `{magic, version, source content hash, payload size}`. Each domain carries a manifest (`cooked/manifest.json`, updated on every save) mapping `<type>:<source name>` to the artifact file, version and source content hash — the lookup authority. Lookup order on request:

1. `Path::Resource` — packaged, produced at build time.
2. `Path::Internal` — produced by a previous run of this installation.
3. Miss, version mismatch or invalid header: the job runs and saves to internal storage.

A request whose key has no source hash performs a logical lookup and validates against the hash recorded in the manifest internally. Zero remains a valid resolved hash. If the logical path misses, the worker-side factory loads the source and computes its content hash; the cooker then searches same-type artifacts by version and content before executing the job. This lets relocated but identical assets — such as a sky map copied beside a USD export — reuse an artifact without reducing identity to a collision-prone filename.

Invalidation is automatic: bump the job's `version` when the algorithm or payload layout changes, and the source content hash covers asset edits where the source exists. The `rebuild_cache` config forces a miss on every lookup.

Saves are atomic: artifact and manifest are written to a temporary file and renamed into place, so a crash or a concurrent reader never observes partial content.

Artifact payloads for full GPU images use the `RHIImage::Upload`/`ReadToMemory` byte order, which is a cross-backend contract: mip-major with array layers inside each mip, rows tightly packed. `VulkanImage` and `MetalImage` must never diverge on this.

## CPU/GPU parity

A job with a GPU-accelerated producer runs it only when a physical GPU is present (`RHIContext::HasPhysicalGpu`; software rasterizers such as lavapipe take the CPU jobs) — both producers must emit the same artifact under the same key.

The CPU jobs port the cook shaders one-to-one ([libraries/include/renderer/resource/IblCookMath.h](../libraries/include/renderer/resource/IblCookMath.h) mirrors `shaders/include/{math,random,sampler,cubemap}.h.slang`), including the shader RNG — GPU sample seeds are contiguous per pixel regardless of dispatch batching, so the CPU integrates the identical sample set — plus hardware sampling behavior: the `uv * size - 0.5` texel-center convention and seamless cube edge taps (out-of-face bilinear taps resolve geometrically to the adjacent face).

The `ibl_parity` test case enforces this: it deletes the internal IBL artifacts, lets the GPU cook them, runs the CPU jobs and compares (rgb only — the alpha channel is producer-dependent and no consumer reads it). It is deliberately not a CI gate — the CPU reference cook costs ~15 runner-minutes, and once a CI node cooks through the CPU jobs the screenshot gates cover divergence end to end. It runs as part of the local macos suite (dev/run_tests.py includes it for the macos framework only; a software rasterizer would compare CPU to CPU) or standalone:

```bash
python3 build.py --framework macos --config Release --run --test_case ibl_parity --headless true
```

On failure it writes both payloads to internal storage under `cooked_debug/` for offline analysis. Measured on Apple M-series: `ibl_brdf` max error is one half ULP; the env maps stay under 0.05 against a clamp ceiling of 10.

When editing any `shaders/screen/ibl_*.cs.slang` or the shared shader includes, mirror the change in the CPU port and bump the affected artifact version. Versions live in the job classes ([IblBrdfCookJob.h](../libraries/include/renderer/resource/IblBrdfCookJob.h), [IblEnvCookJobs.h](../libraries/include/renderer/resource/IblEnvCookJobs.h)); shared CPU/GPU algorithm settings live in [IblSettings.h](../libraries/include/renderer/resource/IblSettings.h), not in either producer or a render proxy.

## Release cooking

The bundled cook list, [resources/packed/config/cook_list.json](../resources/packed/config/cook_list.json), declares which scenes contribute derived artifacts to a release cook — currently `TestScene.usda`. The application supplies the cook plan explicitly: the scene-independent BRDF job is always included, and a successfully resolved scene sky contributes the diffuse and specular environment jobs. It does not define the package's raw-resource closure; the normal build packages resources independently.

The tradeoff of additive cooking is package size: every package carries both the source assets and their cooked artifacts — with the current jobs, roughly 130 MB of RGBA16F payloads per sky map (48 MB skylight cube, 67 MB specular chain, 12 MB diffuse, 2 MB BRDF LUT) on top of the source HDR. This is acceptable for the current content set. When content grows, the levers are artifact compression and per-platform compressed texture formats (which introduce the platform key dimension); stripping sources from packages is deliberately not one — dev features such as USD export need them, and cooking stays additive by design.

build.py exposes the pipeline as stages (see [Build.md](Build.md)): `--stage cook` runs a host cook, and the package stage injects the cooked directory into the product archive through [dev/inject_cooked.py](../dev/inject_cooked.py) — the same module CI release nodes use. Packaging carries no cooked-content policy: the per-job runtime fallback covers missing content, and the CI test stage's log gate is what verifies that shipped packages never actually take that fallback.

## CI

CI cooks once per pipeline run on a macos-release node (a real Metal GPU cooks in seconds, unlike software Vulkan) and injects the result into every product's release package; the test stage's log gate fails any run that cooks on the fly. A single shared cook serves all platforms because the image payload layout is a cross-backend contract (see Artifacts); genuinely platform-specific content would add per-platform cook nodes. The stage graph, injection and re-signing rules live in [CI.md](CI.md).
