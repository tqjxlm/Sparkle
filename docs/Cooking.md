# Cooking

Cooking moves deterministic asset work out of the frame loop: a cook job transforms source data into a validated disk artifact once, and every later run loads the artifact.

Day to day it is automatic. `run.py` and the default `build.py` invocation run the cook stage after building (see [Build.md](Build.md) for stage selection, host/target rules and packaging), and at runtime a missing artifact cooks on the fly while the requester shows its pending state. Artifacts live in the app's internal storage under `cooked/` (`sparkle.app/Contents/SharedSupport/cooked` for macos, `build/generated/cooked` for glfw); delete that directory or run the app with `--rebuild_cache true` to force a recook. Cook stage runs log to `logs/cook.log` next to the regular app logs. Device frameworks (`android`, `ios`) cannot read the host's cook output at runtime, so a `run.py` launch for them also packages: the product's asset tree is replaced with the cooked content image, the package is re-signed, and the run installs that product — the device then reads the packaged artifacts instead of cooking on the fly.

## Architecture

* `CookJob` ([libraries/include/core/cook/CookJob.h](../libraries/include/core/cook/CookJob.h)): one deterministic unit of work. `Execute()` runs off the main thread and must stay CPU-only (no RHI, no scene access), so jobs can run in a render-less process. It returns an explicit `CookJobResult`.
* `CookArtifactStore` ([libraries/include/core/cook/CookArtifactStore.h](../libraries/include/core/cook/CookArtifactStore.h)): owns manifest lookup, artifact validation and persistence across packaged and internal domains. It is independent of scheduling and rendering.
* `Cooker` ([libraries/include/core/cook/Cooker.h](../libraries/include/core/cook/Cooker.h)): orchestrates requests. `Request(job, on_ready)` handles an already-constructed job. `Request(key, job_factory, on_ready)` resolves the logical key first and constructs the source-dependent job on a dedicated thread only after a miss. Cache hits and fresh cooks always return the same `CookHandle` and deliver a `CookResult` on the main thread; destroying the handle cancels delivery, so requesters cannot be called back after death. Concurrent requests for the same lookup key share one execution. Cache metadata stays inside the cook layer; it does not leak into scene or rendering APIs.
* `SceneCooker` ([libraries/include/scene/cook/SceneCooker.h](../libraries/include/scene/cook/SceneCooker.h)) owns scene loading and build-time execution above the core artifact store. Its caller supplies an explicit `JobPlan`; runtime scene objects expose no build interfaces. Scene loading, asynchronous resource resolution, plan collection, execution and store failures all reach the process exit code.
* IBL owns its job declaration in [IblCookPlan](../libraries/include/renderer/resource/IblCookPlan.h) and optional GPU execution in [IblCookAccelerator](../libraries/include/renderer/resource/IblCookAccelerator.h). The application composition boundary connects the resolved scene sky to that plan. GPU passes and the accelerator only generate payloads; they do not know manifests or package policy. Core cooking has no RHI or renderer configuration dependency.

The cook stage's product is one self-contained content image per cook target: every raw asset the target's plan does not replace passes through unchanged — a cooked artifact can simply be its raw source — plus exactly the plan's derived artifacts under `cooked/`. Packaging replaces a product's asset tree with its own target's image rather than merging into it; only the build-owned shader tree stays. Runtime stays tolerant per job: a package with missing or partial cooked content works because every job falls back to its packaged raw source and cooks on the fly.

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

Invalidation is automatic: bump the job's `version` when the algorithm or payload layout changes, and the source content hash covers asset edits where the source exists. The `rebuild_cache` config skips the writable internal store but continues to read packaged artifacts, which cannot be rebuilt in place. A source-backed development run therefore re-encodes when no packaged artifact exists, while a stripped package keeps using its build-time artifact.

Saves are atomic: artifact and manifest are written to a temporary file and renamed into place, so a crash or a concurrent reader never observes partial content.

Artifact payloads for full GPU images use the `RHIImage::Upload`/`ReadToMemory` byte order, which is a cross-backend contract: mip-major with array layers inside each mip, rows tightly packed. `VulkanImage` and `MetalImage` must never diverge on this.

## Material texture compression

Material textures (base color, normal, metallic-roughness, emissive from glTF/USD scenes) cook into block-compressed artifacts with full mip chains, replacing the runtime-decoded single-mip RGBA8 uploads. This cuts GPU memory roughly 3-6x, adds proper minification, and lets packages ship without the source images.

Two artifact families exist because desktop GPUs have no ASTC and mobile GPUs prefer ASTC over BC: `texture_astc` (Apple via Metal or MoltenVK, Android) and `texture_bc` (Windows/Linux Vulkan). Formats per profile: `color` (sRGB: base color, emissive) and `data` (linear: metallic-roughness) use ASTC 6x6 / BC7; `normal` uses ASTC 4x4 / BC7 and its mips are renormalized. Mips are generated at cook time with a gamma-correct box filter — block-compressed images cannot be blit-downsampled at runtime. The encoders are [astc-encoder](https://github.com/ARM-software/astc-encoder) and [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo); presets and block sizes live in [TextureCompression](../libraries/include/io/TextureCompression.h).

The artifact identity is `<packed-relative image path>#<profile>`; its source hash combines the decoded RGBA8 pixels with the profile so content-alias reuse cannot cross color, data and normal encodings. `TextureCookJob` owns the job. Identities exist only for packaged (`Path::Resource`) non-embedded images — exported, external and procedural textures always load raw. Identity-only lookup does not observe source-image edits in a runtime-only development loop: a warm artifact continues to resolve by name until the cook stage runs again or `rebuild_cache` forces a source-backed re-encode.

Unlike the async SkyLight pattern, texture cooks resolve synchronously inside the scene-load worker (`ResolveMaterialTexture` via `Cooker::CookNow`): scene loading already carries the async completion contract that tests fence on, and resolving in place avoids a pending state, proxy recreation and bindless churn. A cache hit swaps the compressed payload in; a miss encodes inline (seconds, and only on runs without a prior cook stage); a source with neither artifact nor pixels falls back to a dummy texture with an error.

The compressed image stays a regular `Image2D`. Consumers that need texels — the CPU pipeline's material sampling and USD export — decode lazily to an RGBA8 shadow copy on first use. If a device cannot sample the format (`RHIContext::SupportsSampledFormat`), `CreateTexture` uploads that decoded copy instead; software rasterizers (lavapipe, SwiftShader) sample both families directly.

Only the requested cook targets' families cook (the target→family mapping is `CookTargetFamilies` in AppFramework.cpp); a build-time cook writes `cooked/cook_products.json`, mapping each target to its plan's manifest keys and to the source images those artifacts fully replace. Image assembly (`assemble_cooked_image` in build.py) projects the pool through that file: a target's image carries exactly its plan's artifacts — never the other family — and a replaced source is left out only when every replacing artifact verifies (an unverifiable source ships and stays the runtime fallback). Packaging ([dev/package_cooked.py](../dev/package_cooked.py)) swaps in the product's own image and asserts its self-declared target instead of carrying any content policy. Inside a stripped package the image load fails, tydra keeps the authored path on the image entry, and the slot resolves from the manifest by identity alone — with no source pixels an inline cook is impossible, so a missing artifact degrades to the dummy texture instead of poisoning the cache. `rebuild_cache true` in a stripped package cannot re-encode textures — it keeps using the packaged artifacts.

The `texture_compression` test case covers encode/decode invariants for every profile and family on every platform; packaged screenshot runs plus the CI cook gate prove end to end that shipped packages resolve every texture from artifacts.

## CPU/GPU parity

A job with a GPU-accelerated producer runs it only when a physical GPU is present (`RHIContext::HasPhysicalGpu`; software rasterizers such as lavapipe take the CPU jobs) — both producers must emit the same artifact under the same key.

The CPU jobs port the cook shaders one-to-one ([libraries/include/renderer/resource/IblCookMath.h](../libraries/include/renderer/resource/IblCookMath.h) mirrors `shaders/include/{math,random,sampler,cubemap}.h.slang`), including the shader RNG — GPU sample seeds are contiguous per pixel regardless of dispatch batching, so the CPU integrates the identical sample set — plus hardware sampling behavior: the `uv * size - 0.5` texel-center convention and seamless cube edge taps (out-of-face bilinear taps resolve geometrically to the adjacent face).

The `ibl_parity` test case enforces this: it deletes the internal IBL artifacts, lets the GPU cook them, runs the CPU jobs and compares (rgb only — the alpha channel is producer-dependent and no consumer reads it). It is deliberately not a CI gate — the CPU reference cook costs ~15 runner-minutes, and once a CI node cooks through the CPU jobs the screenshot gates cover divergence end to end. Its `recooks` mark keeps it out of coverage runs (the deliberate recook would trip the suite's cook gate; it also needs a physical GPU — a software rasterizer would compare CPU to CPU; see [Test.md](Test.md)), so it runs via `--case ibl_parity` on macos, or standalone:

```bash
python3 run.py --framework macos --config Release --test_case ibl_parity --headless true
```

On failure it writes both payloads to internal storage under `cooked_debug/` for offline analysis. Measured on Apple M-series: `ibl_brdf` max error is one half ULP; the env maps stay under 0.05 against a clamp ceiling of 10.

When editing any `shaders/screen/ibl_*.cs.slang` or the shared shader includes, mirror the change in the CPU port and bump the affected artifact version. Versions live in the job classes ([IblBrdfCookJob.h](../libraries/include/renderer/resource/IblBrdfCookJob.h), [IblEnvCookJobs.h](../libraries/include/renderer/resource/IblEnvCookJobs.h)); shared CPU/GPU algorithm settings live in [IblSettings.h](../libraries/include/renderer/resource/IblSettings.h), not in either producer or a render proxy.

## Release cooking

The bundled cook list, [resources/packed/config/cook_list.json](../resources/packed/config/cook_list.json), declares which scenes contribute derived artifacts to a release cook. It includes the shipped `TestScene.usda` content and the nested USD/external-image glTF fixtures that gate packaged texture resolution. The application supplies the cook plan explicitly: the scene-independent BRDF job is always included, and a successfully resolved scene sky contributes the diffuse and specular environment jobs. It does not define the package's raw-resource closure; the normal build packages resources independently.

The tradeoff of pass-through cooking is package size: the image carries both the source assets and their derived artifacts — with the current jobs, roughly 130 MB of RGBA16F payloads per sky map (48 MB skylight cube, 67 MB specular chain, 12 MB diffuse, 2 MB BRDF LUT) on top of the source HDR. This is acceptable for the current content set. A material texture source is left out of the image once every replacing artifact verifies; sources for jobs without that replacement contract remain in the image. The image owns what ships, so source removal is a cook decision, not a packaging one.

build.py exposes the pipeline as stages (see [Build.md](Build.md)): `--stage cook` runs a host cook and assembles one content image per requested cook target, and the package stage swaps the product's own image into its archive through [dev/package_cooked.py](../dev/package_cooked.py) — the same module CI release nodes use — then re-signs packages whose signature the rewrite broke. A package without an image is an error: packages always carry cooked content, and the CI test stage's log gate verifies that shipped packages never take the per-job runtime fallback.

## CI

CI cooks once per pipeline run on a macos-release node (a real Metal GPU cooks in seconds, unlike software Vulkan): one multi-target invocation covering every shipped cook target, publishing one content image per target, and each release package receives its own target's image; the test stage runs the released windows and linux packages under software Vulkan and the released macos packages (macos framework, and glfw through MoltenVK) on a Metal GPU, each with a log gate that fails any run cooking on the fly. A single cook node serves all targets because the artifact pool is shared and the union plan costs no more than any one target's; if per-target cook times ever diverge, per-target cook nodes are the natural split. The stage graph, injection and re-signing rules live in [CI.md](CI.md).
