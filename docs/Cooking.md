# Cooking

Cooking moves deterministic asset work out of the frame loop: a cook job transforms source data into a validated artifact once, and every later run loads the artifact.

Day to day it is automatic. `run.py` and the default `build.py` run the cook stage after building ([Build.md](Build.md#pipeline-stages)), and at runtime a missing artifact cooks on the fly while its requester shows a pending state. Runtime artifacts live in internal storage under `cooked/` (`build/generated/cooked` in the glfw output, `sparkle.app/Contents/SharedSupport/cooked` for macos); delete it or run with `--rebuild_cache true` to recook. Device frameworks cannot read the host's cook output, so they receive cooked content through their package.

## Architecture

* `CookJob` ([CookJob.h](../libraries/include/core/cook/CookJob.h)): one deterministic unit of work. `Execute()` runs off the main thread and stays CPU-only (no RHI, no scene access), so jobs can run in a render-less process. It returns an explicit `CookJobResult`.
* `CookArtifactStore` ([CookArtifactStore.h](../libraries/include/core/cook/CookArtifactStore.h)): manifest lookup, artifact validation and persistence across the packaged and internal domains, independent of scheduling and rendering.
* `Cooker` ([Cooker.h](../libraries/include/core/cook/Cooker.h)): orchestrates requests. `Request(key, job_factory, on_ready)` resolves the logical key first and constructs the source-dependent job on a dedicated thread only after a miss; `Request(job, on_ready)` takes a constructed job. Hits and fresh cooks both deliver a `CookResult` on the main thread through a `CookHandle`, whose destruction cancels delivery. Concurrent requests for one key share one execution. Cache metadata never leaks into scene or rendering APIs.
* `SceneCooker` ([SceneCooker.h](../libraries/include/scene/cook/SceneCooker.h)) loads scenes and executes an explicit `JobPlan` at build time; every failure reaches the process exit code.
* IBL declares its jobs in [IblCookPlan](../libraries/include/renderer/resource/IblCookPlan.h) and accelerates them on the GPU in [IblCookAccelerator](../libraries/include/renderer/resource/IblCookAccelerator.h). GPU passes only generate payloads; core cooking has no RHI or renderer dependency.

## Artifacts

An artifact lives at `cooked/<type>/<source stem>_<name hash>.cook`, named by a hash of the normalized logical source path so same-named assets do not collide, with a header `{magic, version, source content hash, payload size}`. Each domain's `cooked/manifest.json` maps `<type>:<source name>` to the artifact file, version and source hash, and is the lookup authority. A request looks up:

1. `Path::Resource`: packaged, produced at build time.
2. `Path::Internal`: produced by an earlier run of this installation.
3. On a miss, version mismatch or invalid header, the job runs and saves to internal storage.

* When the logical path misses, the factory loads the source and hashes its content, and the cooker searches same-type artifacts by version and content before cooking. A relocated but identical asset, such as a sky map copied beside a USD export, reuses the artifact.
* Invalidation is automatic: bump the job's version when its algorithm or payload layout changes; the source hash covers asset edits.
* `rebuild_cache` skips the internal store but still reads packaged artifacts, which cannot be rebuilt in place.
* Saves are atomic (temporary file, then rename), so a crash or concurrent reader never sees partial content.
* GPU image payloads use the `RHIImage::Upload` / `ReadToMemory` byte order, a cross-backend contract: mip-major, array layers inside each mip, rows tightly packed.

## Adding a Cook Job

1. Implement `CookJob`: a stable `GetType()` (also the artifact directory), a `GetVersion()` to bump with the algorithm or layout, `GetSourceName()` / `GetSourceHash()` as the content identity, and a CPU-only `Execute()` (it may fan out through `TaskManager::ParallelFor`).
2. Request it at its natural trigger with `Cooker::Request`, preferably the key and factory overload, so a hit never decodes the source.
3. Give the requester a valid pending state: rendering must work before delivery.
4. Register the request as a scene async task, so screenshot tests and USD export wait for it. `SkyLight::RequestCook` is the reference for the request, deliver and apply pattern.
5. Add the job to the build-time plan (`RunCookPipeline` in [CookPipeline.cpp](../libraries/source/application/CookPipeline.cpp)), and to [cook_list.json](../resources/packed/config/cook_list.json) if it needs a scene not cooked yet.
6. A GPU producer must emit byte-identical artifacts under the same key (see [CPU/GPU Parity](#cpugpu-parity)).

## Content Images and Packaging

A build-time cook produces one self-contained content image per cook target (`--cook_targets`, default the built framework's own), at `build_system/<cooker>/output/cooked_image/<target>`. The artifact pool is shared across targets; [cook_targets.json](../cook_targets.json) maps each target to its texture family, read by CMake, `build.py` and the CI generator.

* The cook writes `cooked/cook_products.json`, mapping each target to its plan's manifest keys and to the sources those artifacts fully replace. Image assembly (`assemble_cooked_image` in [build.py](../build.py)) projects the pool through it: an image carries exactly its plan's artifacts, every other packed asset as itself, and leaves out a source only when every replacing artifact verifies.
* Each image declares its target in `cooked/content_target.json`. The package stage ([dev/package_cooked.py](../dev/package_cooked.py), shared with CI release nodes) replaces the archive's asset tree with the product's own image (`--cooked <dir>` overrides), keeps the build-owned shaders, strips internal runtime state, asserts the declared target and re-signs: apk with zipalign and apksigner using the debug key, ipa with the identity that signed the build.
* [cook_list.json](../resources/packed/config/cook_list.json) lists the scenes that contribute derived artifacts: the shipped TestScene and the fixtures that gate packaged texture resolution. The scene-independent BRDF job is always included.
* Runtime stays tolerant: a job whose artifact is missing falls back to its packaged source and cooks on the fly, which the CI test stage's cook gate forbids for shipped packages.
* Images carry sources next to derived artifacts unless an artifact replaces its source; about 16 MB of derived HDR data per sky map ships on top of the source HDR.

## Material Texture Compression

Material textures (base color, normal, metallic-roughness, emissive) cook into block-compressed artifacts with full mip chains, cutting GPU memory about 3 to 6 times and letting packages ship without the source images.

* Two families exist because desktop GPUs lack ASTC and mobile GPUs prefer it: `texture_astc` (Apple, Android) and `texture_bc` (Windows and Linux Vulkan). The `color` (sRGB) and `data` (linear) profiles use ASTC 6x6 or BC7, `normal` uses ASTC 4x4 or BC7 with renormalized mips. Mips are generated at cook time with a gamma-correct box filter, since compressed images cannot be blit-downsampled. Encoders, presets and block sizes are in [TextureCompression](../libraries/include/io/TextureCompression.h).
* `TextureCookJob` identifies an artifact as `<image identity>#<profile>`: a packaged file by its packed-relative path, an image embedded in a `.glb` or `data:` URI by `<scene parent>/@embedded-<content hash>` (additive, since there is no source file to replace). Exported, external and procedural textures load raw.
* Texture cooks resolve synchronously inside the scene-load worker (`Cooker::CookNow`), which avoids a pending state and bindless churn. A miss encodes inline; a source with neither artifact nor pixels, as in a stripped package, becomes a dummy texture with an error.
* Identity-only lookup does not notice source edits in a runtime-only loop; run the cook stage or `rebuild_cache` to re-encode.
* Consumers that need texels (the cpu pipeline, USD export) decode lazily to an RGBA8 copy, which `CreateTexture` also uploads when a device cannot sample the format (`RHIContext::SupportsSampledFormat`).

## Sky and IBL HDR Compression

Packaged targets ship the sky cube and the IBL diffuse and specular cubes as ASTC-HDR 4x4 (Apple, Android) or BC6H (desktop), both 8 bpp, instead of `RGBAFloat16`. Equal bitrates keep the families within about 0.008 mean FLIP of each other and of the fp16 masters, so one ground truth serves every family. BC6H encodes through [ispc_texcomp](https://github.com/GameTechDev/ISPCTextureCompressor) and re-encodes blocks above 5% round-trip error with the exhaustive [ConvectionKernels](https://github.com/elasota/ConvectionKernels) search, which is also the decoder everywhere. Frameworks that cannot cook (android, ios) build only ConvectionKernels.

* The expensive producers, the sky projection (`skylight`) and the IBL integrations (`ibl_diffuse`, `ibl_specular`), cook once per sky map into fp16 masters that stay in the pool and never ship. `HdrCubeTranscodeJob` derives one cheap family artifact per family from each master (`skylight_astc`, `ibl_specular_bc`, ...). The BRDF LUT stays `RGBAFloat16`.
* A transcode's source hash (`HdrCubeTranscodeJob::MakeSourceHash`) is the sky map's source hash for the sky, and the family sky cube's encoded bytes for IBL, each mixed with the master version. The fp16 master cannot key anything, because the projection is not bit-deterministic across platforms.
* At runtime the slots look up the family transcode first, then the fp16 master. A full miss cooks the master, GPU-accelerated on a physical GPU, and re-probes the transcode with the master's resolved source hash; block encoders never run outside the build cook. `HdrCubeTranscodeJob::Version` invalidates every transcode at once.
* Release cooks parallelize block encoding over bands of block rows and run family transcodes as concurrent jobs, bounded by master memory.

## CPU/GPU Parity

A GPU producer runs only on a physical GPU (`RHIContext::HasPhysicalGpu`); software rasterizers take the CPU jobs. Both must emit the same artifact under the same key.

* The CPU jobs port the cook shaders one-to-one ([IblCookMath.h](../libraries/include/renderer/resource/IblCookMath.h) mirrors the shared `shaders/include/` math, random, sampler and cubemap headers), including the shader RNG's per-pixel seeds and hardware sampling behavior: the `uv * size - 0.5` texel-center convention and seamless cube-edge taps.
* When editing a `shaders/screen/ibl_*.cs.slang` shader or the shared includes, mirror the change in the CPU port and bump the artifact version in [IblBrdfCookJob.h](../libraries/include/renderer/resource/IblBrdfCookJob.h) or [IblEnvCookJobs.h](../libraries/include/renderer/resource/IblEnvCookJobs.h). Shared settings live in [IblSettings.h](../libraries/include/renderer/resource/IblSettings.h).
* The `ibl_parity` case deletes the internal IBL artifacts, cooks them on the GPU and through the CPU jobs, and compares the fp16 masters (rgb only). Tolerances: half an ULP for `ibl_brdf`, 0.05 against a clamp ceiling of 10 for the environment maps. On failure it writes both payloads under `cooked_debug/` in internal storage. It needs a physical GPU and costs about 15 runner-minutes, and its deliberate recook would trip the cook gate, so it is not a CI gate; run it on macos with `python3 dev/run_tests.py --framework macos --config Release --case ibl_parity`.
