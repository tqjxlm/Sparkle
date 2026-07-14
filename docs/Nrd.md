# NRD denoiser integration

NVIDIA NRD (ReBLUR_DIFFUSE_SPECULAR) denoises the gpu path tracer: `--pipeline gpu --nrd true`. Supported on Apple platforms (macOS + iOS) and Android — see [Platform support](#platform-support).

## Architecture

```text
ray_trace.cs.slang        WritePrimaryHitGBuffer: noise-free primary-hit G-buffer
                          (radiance+hitT, normal+viewZ, albedo+objID, motion+isHit, specAlbedo+roughness)
        |
nrd_pack.cs.slang         demodulate diffuse (albedo) / specular (pre-integrated BRDF), normalize hit
                          distances, sky sentinel viewZ; writes NRD's IN_* textures
        |
NrdDenoiser               owns the GPU-agnostic nrd::Instance; per frame feeds CommonSettings
 (renderer)               (matrices, frame index, reset) and translates GetComputeDispatches into
        |                 RHINrdBackend dispatches
MetalNrdBackend /         Metal loads NRD's pre-cooked MSL (see below) and binds resources by NRD
VulkanNrdBackend          register order on one encoder; Vulkan consumes NRD's SPIR-V directly,
        |                 reflecting bindings on device with spirv_reflect
        |
nrd_resolve.cs.slang      re-modulate, composite, convergence handoff to the accumulator, output
                          stabilization EMA; also renders the --nrd_debug views
```

Key files: [NrdDenoiser](../libraries/include/renderer/nrd/NrdDenoiser.h), [NrdCookedShaders](../libraries/include/renderer/nrd/NrdCookedShaders.h), [RHINrdBackend](../libraries/include/rhi/RHINrdBackend.h) with its [Metal](../libraries/source/rhi/metal/MetalNrdBackend.mm) and [Vulkan](../libraries/source/rhi/vulkan/VulkanNrdBackend.cpp) implementations, and the pack/resolve shaders in [shaders/nrd/](../shaders/nrd/).

NRD has no Metal backend of its own (NRI supports D3D/Vulkan only), so the integration uses NRD's "native API" path: the C++ library only describes pipelines/dispatches; the engine executes them. The encoding replicas in [shaders/include/nrd.h.slang](../shaders/include/nrd.h.slang) are pinned to the NRD version (`thirdparty/NRD`, see `NRD_NORMAL_ENCODING` in `thirdparty/CMakeLists.txt`).

## Cooked shaders

NRD ships its shaders as SPIR-V; the cook filters and repacks them at BUILD time into the intermediate shader directory alongside the engine's own shaders (nothing committed). The cook lives in [shaders/nrd/cook/](../shaders/nrd/cook/) and is wired into [shaders/CMakeLists.txt](../shaders/CMakeLists.txt):

1. NRD's build (`NRDShaders`) generates ShaderMake blob headers with every shader permutation.
2. [cook_nrd_shaders.py](../shaders/nrd/cook/cook_nrd_shaders.py) unpacks them and filters to the permutations the engine uses (REBLUR + Clear, `NRD_SIGNAL=BOTH`).
   * On macos/ios it drives [nrd_msl_cook](../shaders/nrd/cook/nrd_msl_cook.cpp) — a small build-host tool (compiled with the Vulkan SDK's spirv-cross static libs, host-run even when targeting iOS) that emits MSL plus the reflection metadata (entry, workgroup size, binding->MSL index maps) into a manifest.
   * On android it repacks the SPIR-V; only the entry point and workgroup size go into the manifest, and `VulkanNrdBackend` reflects bindings on device with spirv_reflect (already linked for engine shaders). Two byte-level rewrites make NRD's DXC output (vulkan1.2 target) legal on the engine's Vulkan 1.1 instance, each proven per module with spirv-val at cook time: the version word drops from SPIR-V 1.5 to 1.4, and the `SPV_KHR_compute_shader_derivatives` declarations are stripped — DXC adds them only to pin `QuadReadAcross*` in compute to 2x2 quads, and the resulting linear quad mapping is the semantics the Metal backend already ships with (simd-group quads).
3. The cooked set is packed into app resources; the runtime (`NrdCookedShaders` loader + backend) looks pipelines up by a canonical form of `PipelineDesc::shaderIdentifier` and builds the pipeline like any other engine shader.

The manifest carries the NRD version; `NrdDenoiser` hard-fails on a mismatch or a missing permutation (extend the filters in `shaders/CMakeLists.txt` if NRD ever needs one the cook skips). The cook re-runs automatically when NRD's generated shaders change.

## Conventions that must hold

* NRD decomposes `viewToClipMatrix` with D3D clip conventions (+Y up). The engine's projection bakes a Vulkan-style Y flip, so `NrdDenoiser` negates row 1 before the handoff. Getting this wrong is invisible for static views and yaw motion and breaks pitch/roll/vertical motion (the pitch gate exists for exactly this).
* viewZ is signed view-space z (negative for visible geometry); sky uses a sentinel beyond `denoisingRange`.
* Skipped-lobe frames write hit distance EXACTLY 0 (`HitDistanceReconstructionMode` fills them); any positive floor collapses ReBLUR's blur radius.
* Sky pixels write a valid placeholder normal: ReBLUR's temporal accumulation averages neighbor normals without range gating, so NaN corrupts adjacent geometry.

## Performance

Each NRD stage (pack, the ReBLUR dispatch block, resolve) is a timestamped `RHIComputePass`; the per-stage GPU times appear in the log as `[NrdPerf]` running/final averages. Apple GPUs sample timestamps at encoder boundaries only, so the ReBLUR block reports as one number — use Xcode GPU capture (`NrdReblurPass`) for a per-pass breakdown. Once the convergence handoff (next section) completes, the ReBLUR block is skipped and a static view pays only the resolve.

Tuning:

* `--nrd_stabilization` (default on): ReBLUR's temporal stabilization pass. Off saves one full-res pass at the cost of visibly noisier motion; the resolve's output EMA does not compensate, because it intentionally decays to zero under motion.
* `--nrd_radiance_fp16` (default on): RGBA16F radiance G-buffers, half the pack-stage bandwidth. Turn off (fp32) to rule out radiance/hitT quantization when debugging.
* `NRD_NORMAL_ENCODING` (build-time, `thirdparty/CMakeLists.txt`): `2` oct-packs IN_NORMAL_ROUGHNESS into R10G10B10A2, halving ReBLUR's most-sampled texture; `4` stores raw RGBA16 floats — switch back to rule out the packing when debugging normal-related artifacts.

ReBLUR's transient pool is allocated as discrete textures: all 8 lifetimes overlap within the frame's pass chain, so heap aliasing cannot shrink it.

## Display pipeline: handoff and freeze

ReBLUR's capped history re-blends fresh noise forever, so a static view would shimmer. The resolve cross-fades to the progressive accumulator over `[HandoffStartSamples, HandoffEndSamples]` (scaled down to max_spp when smaller, skipped entirely when max_spp is below the window so the max_spp=1 motion harnesses keep filming ReBLUR output). An output-side EMA suppresses residual pops and fades out with the handoff weight, so the display already equals the accumulator when the window closes (a lagging EMA would otherwise release its backlog as a visible pop at the freeze). When a frame's samples complete the max_spp target, that final resolve runs with weight 1 and EMA 0: the frozen frame equals the accumulator bit-exactly (asserted by the test suite). The RNG seed advances every dispatch, never resets on accumulator clears (seed pinning defeats temporal denoising), and restarts once when the scene finishes loading so headless captures are bit-reproducible.

## Tests

[tests/nrd/run_nrd_gates.py](../tests/nrd/run_nrd_gates.py) is the one-command suite, run locally before pushing and by the CI macos-release test node (the hosted macos runners' Apple Silicon GPUs support Metal ray tracing; no other CI runner has hardware ray tracing): converged flicker, firefly, motion noise (yaw + pitch arms). The probe run requires the app's `effective pipeline: Gpu` log marker — on GPUs without hardware ray tracing the app silently falls back to forward rendering and still exits 0, so exit codes alone prove nothing. Motion gates assert all captures are pairwise distinct (a frozen/black capture sequence otherwise passes statistical thresholds). `--test_case nrd_probe` checks that every ReBLUR pipeline compiles to a PSO through the production MetalNrdBackend path.

## Platform support

`MetalNrdBackend` runs on macOS and iOS (`FRAMEWORK_APPLE`): the runtime consumes cooked MSL; spirv-cross runs only at build time, inside the `nrd_msl_cook` host tool. ReBLUR needs SIMD-group ops (Apple A13+/M1+ GPUs) and the gpu pipeline needs the device's ray-tracing support — the existing capability gates and `--test_case nrd_probe` cover both per device.

`VulkanNrdBackend` runs on Android: it consumes NRD's SPIR-V directly, keeps NRD's pool textures in `VK_IMAGE_LAYOUT_GENERAL` for their whole life (inter-dispatch hazards become plain compute-to-compute memory barriers, matching Metal's serial-encoder semantics), and transitions user-facing IN_*/OUT_* images through the engine's layout tracker. It requires subgroup quad operations in compute (ReBLUR HistoryFix) and the gpu pipeline requires `VK_KHR_ray_query`; a device missing either gets no backend.

RHIs without a backend return nullptr from `RHI::CreateNrdBackend`, which `NrdDenoiser` latches as a permanent init failure.
