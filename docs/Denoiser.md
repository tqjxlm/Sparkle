# Path-Tracing Denoisers

The gpu path tracer can denoise its frame with NVIDIA NRD (ReBLUR) or Apple's MetalFX temporal denoised scaler, behind the provider-neutral `Denoiser` interface. `--denoiser auto` prefers MetalFX where the platform and device support it, then NRD. A provider that fails falls back to NRD, then to the plain progressive accumulator; providers are never chained.

## Settings

| cvar                | default | effect                                                                                                                                        |
| ------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `denoiser`          | `off`   | `off`, `auto`, `nrd` or `metalfx`                                                                                                             |
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
GPURenderer                selects and keeps the effective Denoiser provider
        +-- NrdDenoiser       nrd_pack -> ReBLUR -> nrd_resolve
        +-- MetalFxDenoiser   input preparation -> temporal denoised scaler -> resolve
        |
ToneMappingPass            displays the provider's output, then screenshots, UI and present
```

* Each provider owns its output, temporal history, intermediates and SDK objects, and adds its frame to the gpu renderer's [render graph](RenderGraph.md#renderers) as one External pass (`Denoiser::AddTo`). Its internal textures stay private to the pass and keep their own transitions.
* Provider changes are frame-coherent: a new provider's output shows only in a frame whose trace wrote every input it needs. Turning the denoiser off returns to the accumulator immediately. A provider that cannot encode a frame adds no pass, shows the accumulator and stops being ready, so the next frame selects the fallback.
* A resolution change resizes each provider in place (`Denoiser::Resize`) and restarts its history. A provider that cannot serve the new size, or failed to construct, is retried at the next resize.
* Per-stage GPU times go through `PassTimingAggregator` and appear in the log under each provider's tag, e.g. `[NrdPerf]`.

Key files: [Denoiser](../libraries/include/renderer/denoiser/Denoiser.h), [DenoiserFactory](../libraries/include/renderer/denoiser/DenoiserFactory.h), [PathTracingDenoiserInputs](../libraries/include/renderer/resource/PathTracingDenoiserInputs.h).

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

## Tests

No CI runner has hardware ray tracing ([CI.md](CI.md#hosted-runner-quirks)), so these run locally before pushing a denoiser change.

* [tests/denoiser/run_denoiser_gates.py](../tests/denoiser/run_denoiser_gates.py) runs the gates in one command: converged flicker, fireflies, and motion noise on yaw and pitch. Every gate takes `--denoiser nrd|metalfx` (default `nrd`).
* [tests/denoiser/denoiser_compare.py](../tests/denoiser/denoiser_compare.py) renders a side-by-side provider table (static noise, flicker and FLIP against a converged reference, motion noise, per-frame cost); `--render_scale` covers reconstruction.
* The scripts require the `effective pipeline: Gpu` log marker, because a GPU without ray tracing falls back to forward and still exits 0. Motion gates require pairwise distinct captures, so a frozen or black sequence cannot pass.
* `nrd_probe` checks that every ReBLUR pipeline compiles through `MetalNrdBackend`; `denoiser_handoff` checks the handoff schedule as pure arithmetic, so it runs on every triplet.
