# Path-Tracing Denoisers

The gpu path tracer can denoise its frame with NVIDIA NRD (ReBLUR) or Apple's MetalFX temporal denoised scaler, behind the provider-neutral `Denoiser` interface, and upscale it to output resolution with AMD FSR 3.1, behind the `Upscaler` interface. `--denoiser auto` prefers MetalFX where the platform and device support it, then NRD. A provider that fails falls back to NRD, then to the plain progressive accumulator.

## Reconstruction Providers

A provider that denoises and upscales in one step (MetalFX) is integrated: it serves both stages, so naming it in either `denoiser` or `upscaler` selects it for both. Otherwise the stages are split: a denoiser at scene resolution, then an upscaler from scene to output resolution. [PlanReconstruction](../libraries/include/renderer/denoiser/Reconstruction.h) turns the two settings into the providers to try:

* MetalFX is tried first when either setting names `metalfx`, or when `denoiser` is `auto` and `upscaler` is not `fsr`.
* The split stages are NRD when `denoiser` is not `off`, and FSR when `upscaler` is not `off`. They also serve as MetalFX's fallback.
* A split upscaler runs only when `render_scale` < 1. At native resolution there is nothing to upscale.
* Each split stage falls back on its own: an unavailable denoiser leaves the upscaler on the accumulator, and an unavailable upscaler leaves tone mapping to upsample.
* While a split upscaler follows a denoiser, the path tracer jitters every sample and the primary-hit guides of a frame by one sub-pixel offset from a Halton(2, 3) sequence ([ComputeJitter](../libraries/include/renderer/denoiser/Reconstruction.h)), whose period grows with the square of the upscale factor. The accumulator averages the jitter out over frames, so it still converges to an anti-aliased image. Without a denoiser the upscaler reads the accumulator and nothing is jittered.
* The denoiser passes the jitter through and leaves temporal stabilization to the upscaler, so NRD's output-side moving average is off. The upscaler is told the jitter scaled by one minus the [handoff](#display-handoff) weight, because its input mixes the jittered denoiser output into the unjittered accumulator by that weight.
* The upscaler keeps running after the handoff, fed the accumulator: the converged frame is FSR's temporal output of the converged accumulator.

## Settings

| cvar                | default | effect                                                                                                                                        |
| ------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `denoiser`          | `off`   | `off`, `auto`, `nrd` or `metalfx`                                                                                                             |
| `upscaler`          | `off`   | `off`, `auto`, `fsr` or `metalfx`; a split upscaler needs `render_scale` < 1                                                                  |
| `nrd_radiance_fp16` | true    | RGBA16F noisy-radiance inputs for every provider, half the bandwidth of RGBA32F; turn off to rule out quantization. Needs renderer recreation |
| `nrd_stabilization` | true    | ReBLUR's temporal stabilization pass; off saves one full-resolution pass at the cost of noisier motion                                        |
| `nrd_debug`         | `None`  | NRD channel to visualize instead of the frame                                                                                                 |
| `metalfx_sync_init` | false   | compile the MetalFX scaler synchronously during renderer initialization instead of asynchronously                                             |

`NRD_NORMAL_ENCODING` in [thirdparty/CMakeLists.txt](../thirdparty/CMakeLists.txt) is a build-time choice: `2` packs normals and roughness into R10G10B10A2, halving ReBLUR's most-sampled texture; `4` stores raw RGBA16 floats, to rule out the packing when debugging normal artifacts.

## Architecture

```text
ray_trace.cs.slang         noisy radiance plus noise-free primary-hit auxiliary textures
        |
PathTracingDenoiserInputs  owns the six input textures, allocated only while a provider needs them
        |
GPURenderer                selects and keeps the effective providers
        +-- NrdDenoiser       nrd_pack -> ReBLUR -> nrd_resolve
        |     +-- FsrUpscaler   depth preparation -> FSR 3.1 passes
        +-- MetalFxDenoiser   input preparation -> temporal denoised scaler -> resolve
        |
ToneMappingPass            displays the last provider's output, then screenshots, UI and present
```

* Each provider owns its output, temporal history, intermediates and SDK objects, and adds its frame to the gpu renderer's [render graph](RenderGraph.md#renderers) as one External pass (`Denoiser::AddTo`, `Upscaler::AddTo`). Its internal textures stay private to the pass and keep their own transitions.
* Provider changes are frame-coherent: a new provider's output shows only in a frame whose trace wrote every input it needs. Turning every provider off returns to the accumulator immediately. A denoiser that cannot encode a frame adds no pass and shows the accumulator; any provider that fails stops being ready, so the next frame selects the fallback.
* A resolution change resizes each provider in place (`Denoiser::Resize`) and restarts its history. A provider that cannot serve the new size, or failed to construct, is retried at the next resize.
* NRD's and MetalFX's per-stage GPU times go through `PassTimingAggregator` and appear in the log under each provider's tag, e.g. `[NrdPerf]`. FSR's passes are not timed.

Key files: [Denoiser](../libraries/include/renderer/denoiser/Denoiser.h), [DenoiserFactory](../libraries/include/renderer/denoiser/DenoiserFactory.h), [Upscaler](../libraries/include/renderer/upscaler/Upscaler.h), [UpscalerFactory](../libraries/include/renderer/upscaler/UpscalerFactory.h), [ProviderCache](../libraries/include/renderer/denoiser/ProviderCache.h), [PathTracingDenoiserInputs](../libraries/include/renderer/resource/PathTracingDenoiserInputs.h).

## Display Handoff

Neither provider denoises forever. Over a window of accumulated samples (512 to 2048, scaled down when `max_spp` is smaller) a resolve pass cross-fades the provider output into the progressive accumulator, so the display equals the accumulator when the window closes and the provider stops running. The frame that completes `max_spp` resolves with weight 1, so the final frame equals the accumulator bit-exactly. A `max_spp` below the window opts out, so the `max_spp=1` motion tests keep filming denoised output. `DenoiserHandoff` owns this schedule for both providers, because the tests compare providers against each other.

* NRD adds an output-side moving average that suppresses residual pops and fades out with the handoff weight, so it holds no backlog when the window closes.
* MetalFX matches the accumulator nearest-upsampled at sub-native render scales, exactly as tone mapping upsamples it.
* The path tracer's RNG seed advances every dispatch and never resets on accumulator clears, which would defeat temporal denoising; it restarts once when the scene finishes loading, so headless captures are reproducible.

## NRD

NRD has no Metal backend (its NRI layer supports D3D and Vulkan only), so the integration uses NRD's native API path: the NRD library describes pipelines and dispatches, and `RHINrdBackend` executes them. `MetalNrdBackend` binds NRD's pre-cooked MSL by register order on one encoder; `VulkanNrdBackend` runs NRD's SPIR-V directly and reflects bindings with spirv_reflect. The pack and resolve shaders in [shaders/nrd/](../shaders/nrd/) demodulate and remodulate albedo around ReBLUR.

Key files: [NrdDenoiser](../libraries/include/renderer/nrd/NrdDenoiser.h), [NrdCookedShaders](../libraries/include/renderer/nrd/NrdCookedShaders.h), [RHINrdBackend](../libraries/include/rhi/RHINrdBackend.h) with its [Metal](../libraries/source/rhi/metal/MetalNrdBackend.mm) and [Vulkan](../libraries/source/rhi/vulkan/VulkanNrdBackend.cpp) implementations. The encoding replicas in [shaders/include/nrd.h.slang](../shaders/include/nrd.h.slang) are pinned to the `thirdparty/NRD` version.

### Cooked Shaders

At build time, [shaders/nrd/cook/](../shaders/nrd/cook/) unpacks the shader permutations NRD's build generates, keeps the ones the engine uses (ReBLUR and Clear, `NRD_SIGNAL=NRD_SIGNAL_BOTH`) and packs them with the engine's shaders. For Apple frameworks it converts them to MSL with reflection metadata through the host tool [nrd_msl_cook](../shaders/nrd/cook/nrd_msl_cook.cpp); for Vulkan it repacks the SPIR-V, validating each rewritten module with spirv-val ([cook_nrd_shaders.py](../shaders/nrd/cook/cook_nrd_shaders.py) documents the rewrites). The manifest records the NRD version, and `NrdDenoiser` fails hard on a version mismatch or a missing permutation; extend the filters in [shaders/CMakeLists.txt](../shaders/CMakeLists.txt) when NRD needs one more.

### Conventions That Must Hold

* NRD decomposes `viewToClipMatrix` with D3D conventions (+Y up), while the engine's projection bakes a Vulkan Y flip, so `NrdDenoiser` negates row 1. A mistake is invisible on static views and yaw, and breaks pitch and vertical motion (the pitch gate exists for this).
* `timeDeltaBetweenFrames` is the app's frame time (`RenderConfig::delta_time`): measured interactively, a nominal 1/60 s in headless and test runs. ReBLUR scales anti-lag, history clamping and motion thresholds by it, and at 0 NRD would time frames by wall clock.
* viewZ is signed view-space z, negative for visible geometry; the sky writes a sentinel beyond `denoisingRange`.
* Frames that skip a lobe write hit distance exactly 0 for `HitDistanceReconstructionMode` to fill; any positive floor collapses ReBLUR's blur radius.
* Sky pixels write a valid placeholder normal, because ReBLUR averages neighbor normals without range gating and a NaN corrupts adjacent geometry.
* Primary-hit emissive pixels bypass the denoiser and resolve from the accumulator (`gMotion.z = 2`, sky-sentinel viewZ): emission is terminal and deterministic, and demodulating it would blur its texture onto neighbors.
* Material IDs are 0 (dielectric) and 3 (metal), the exact 2-bit unorm endpoints: ReBLUR compares material IDs by float equality, and mid-range IDs differ by an ulp on Adreno.

### Platforms and Performance

* `MetalNrdBackend` runs on macOS and iOS and needs SIMD-group operations (Apple A13 or M1 and later). `VulkanNrdBackend` runs on every Vulkan build and needs subgroup quad operations in compute; it keeps NRD's pool textures in `VK_IMAGE_LAYOUT_GENERAL`, so its inter-dispatch hazards are plain compute barriers. A device or RHI without a backend latches a permanent NRD init failure.
* Pack, the ReBLUR block and resolve are each a timestamped compute pass. Apple GPUs time encoders only, so ReBLUR reports as one number; use an Xcode GPU capture (`NrdReblurPass`) for a breakdown. After the handoff a static view pays only the resolve.
* ReBLUR's transient pool is allocated as discrete textures: all its lifetimes overlap within the frame, so heap aliasing cannot shrink it.

## MetalFX

MetalFX denoises and reconstructs to output resolution in one step, replacing NRD for the frame. A compute pass splits the shared path-tracer inputs into the color, depth, motion, albedo, normal and roughness textures the scaler expects.

* The color input is the progressive accumulator, not per-frame radiance: the scaler averages in a non-linear space, so high-variance HDR input would settle darker (about 8% on 1 spp), while the accumulator's shrinking noise lets the output converge to it. Path tracer sampling is unchanged, so converged captures with MetalFX equal the plain accumulator.
* Sky and primary-emissive pixels write their radiance as diffuse albedo, so the demodulated signal is flat there.
* Scene changes reset its history; deforming-geometry motion is not supported. A render scale or resolution change recreates the scaler.
* It is available only on the native Metal RHI, when the build SDK has the temporal denoised scaler API and the OS, device, input scale, formats and scaler creation pass the RHI's checks. It is unavailable on the iOS Simulator.

## FSR

FSR 3.1 upscales a split denoiser's output, or the accumulator without one, from scene to output resolution. AMD's host code ([thirdparty/FidelityFX](../thirdparty/FidelityFX/README.md)) drives it through [FfxBackend](../libraries/include/renderer/upscaler/fsr/FfxBackend.h), which executes FidelityFX effects with the engine's RHI, so one implementation serves Vulkan and Metal. Only the Vulkan path runs in CI.

FSR is optional: it is built by default, and `build.py --strip_fsr` (CMake `ENABLE_FSR=OFF`) leaves out FidelityFX, the FSR sources under `renderer/upscaler/fsr/` and the passes in `shaders/fsr/`. Without it, a split upscaler (`upscaler fsr`, or `auto` where MetalFX does not serve) logs that FSR is not part of the build and the frame is not upscaled; the FSR test cases need a build that includes it.

* The passes compile in the engine's shader build from AMD's HLSL ([shaders/fsr/](../shaders/fsr/)), once each, with the options in [fsr3upscaler.h.slang](../shaders/fsr/fsr3upscaler.h.slang): HDR color, render-resolution motion vectors without jitter, depth that grows away from the camera, 32-bit math, and a luma pyramid that synchronizes through groupshared memory. The context flags and the device capabilities FfxBackend reports select exactly that permutation; a pass asking for another fails context creation. Sharpening, reactive mask generation and the debug view are not compiled.
* Each pass's resource table lists every resource its HLSL declares. For SPIR-V, `-preserve-params` with `-O0` keeps them all in the compiled shader, so the bindings stay dense (slang's SPIR-V optimizer strips unused resources despite the flag); Metal binds by name and needs no density, and slangc crashes on that flag for the metal target. FfxBackend reports the tables' names to FSR, which matches its resources to them.
* On Metal, a kernel takes at most 8 read_write textures, and the passes' texture atomics need MSL 3.1 and an Apple6 GPU. AMD's luma pyramid pass binds all six SPD mips but touches only the last one, so [its entry](../shaders/fsr/fsr3upscaler_luma_pyramid.cs.slang) replaces AMD's pass file and declares mip 5 alone; slang would otherwise keep the five unreachable ones.
* FfxBackend creates FSR's resources as RHI images. A format the RHI lacks (8-bit dual channel, 16-bit snorm, packed 11-11-10, two-channel 32-bit) becomes the closest wider float format. Every job transitions the images it touches to the layout of that use, and clears run as compute dispatches.
* The inputs are the color, device depth that `fsr_prepare_depth` derives from the guides' view distance, and the guides' motion vectors, which run from the current to the previous position in uv; sky pixels carry the motion of camera rotation alone. FSR places a pixel's sample at its center minus the jitter, so it receives the jitter negated.
* A resolution or render scale change recreates the FSR context, which restarts its history.

Key files: [FsrUpscaler](../libraries/include/renderer/upscaler/fsr/FsrUpscaler.h), [FfxBackend](../libraries/include/renderer/upscaler/fsr/FfxBackend.h).

## Tests

No CI runner has hardware ray tracing ([CI.md](CI.md#hosted-runner-quirks)), so these run locally before pushing a denoiser change.

* [tests/denoiser/run_denoiser_gates.py](../tests/denoiser/run_denoiser_gates.py) runs the gates in one command: converged flicker, fireflies, and motion noise on yaw and pitch. Every gate takes `--denoiser nrd|metalfx` (default `nrd`).
* [tests/denoiser/denoiser_compare.py](../tests/denoiser/denoiser_compare.py) renders a side-by-side provider table (static noise, flicker and FLIP against a converged reference, motion noise, per-frame cost); `--render_scale` covers reconstruction.
* The scripts require the `effective pipeline: Gpu` log marker, because a GPU without ray tracing falls back to forward and still exits 0. Motion gates require pairwise distinct captures, so a frozen or black sequence cannot pass.
* `nrd_probe` checks that every ReBLUR pipeline compiles through `MetalNrdBackend`; `denoiser_handoff` checks the handoff schedule and `reconstruction_plan` the provider pairing and jitter sequence as pure logic, so they run on every triplet.
* `gpu_nrd_fsr_sync_validation` runs NRD followed by FSR under synchronization validation on the software Vulkan runner. It proves the split path encodes cleanly, not that its image is good: judge FSR's quality locally, e.g. `--denoiser nrd --upscaler fsr --render_scale 0.5`.
