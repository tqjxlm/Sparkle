# NRD denoiser integration

NVIDIA NRD (ReBLUR_DIFFUSE_SPECULAR) denoises the gpu path tracer: `--pipeline gpu --nrd true`.
Apple platforms (macOS + iOS) — see [Platform support](#platform-support).

## Architecture

```
ray_trace.cs.slang        WritePrimaryHitGBuffer: noise-free primary-hit G-buffer
                          (radiance+hitT, normal+viewZ, albedo+objID, motion+isHit, specAlbedo+roughness)
        |
nrd_pack.cs.slang         demodulate diffuse (albedo) / specular (pre-integrated BRDF), normalize hit
                          distances, sky sentinel viewZ; writes NRD's IN_* textures
        |
NrdDenoiser               owns the GPU-agnostic nrd::Instance; per frame feeds CommonSettings
 (renderer)               (matrices, frame index, reset) and translates GetComputeDispatches into
        |                 RHINrdBackend dispatches
MetalNrdBackend           loads NRD's pre-cooked MSL (see below), binds resources by NRD register
 (rhi/metal)              order, runs all dispatches on one encoder
        |
nrd_resolve.cs.slang      re-modulate, composite, convergence handoff to the accumulator, output
                          stabilization EMA; also renders the --nrd_debug views
```

NRD has no Metal backend of its own (NRI supports D3D/Vulkan only), so the integration uses NRD's
"native API" path: the C++ library only describes pipelines/dispatches; the engine executes them.
The encoding replicas in `shaders/include/nrd.h.slang` are pinned to the NRD version
(`thirdparty/NRD`, see `NRD_NORMAL_ENCODING` in `thirdparty/CMakeLists.txt`).

## Cooked shaders

NRD ships its shaders as SPIR-V; the engine consumes them as MSL text, cross-compiled at BUILD
time into the intermediate shader directory alongside the engine's own shaders (nothing on device,
nothing committed). The cook lives in `shaders/nrd/cook/` and is wired into `shaders/CMakeLists.txt`:

1. NRD's build (`NRDShaders`) generates ShaderMake blob headers with every shader permutation.
2. `cook_nrd_shaders.py` unpacks them, filters to the permutations the engine uses (REBLUR +
   Clear, `NRD_SIGNAL=BOTH`), and drives `nrd_msl_cook` — a small build-host tool (compiled with
   the Vulkan SDK's spirv-cross static libs, host-run even when targeting iOS) that emits MSL for
   the target platform plus the reflection metadata (entry, workgroup size, binding->MSL index
   maps) into a manifest.
3. The cooked set is packed into app resources; the runtime (`NrdCookedShaders` loader +
   `MetalNrdBackend`) looks pipelines up by a canonical form of `PipelineDesc::shaderIdentifier`
   and compiles the MSL like any other engine shader — no spirv-cross and no reflection on device.

The manifest carries the NRD version; `NrdDenoiser` hard-fails on a mismatch or a missing
permutation (extend the filters in `shaders/CMakeLists.txt` if NRD ever needs one the cook skips).
The cook re-runs automatically when NRD's generated shaders change.

## Conventions that must hold

* NRD decomposes `viewToClipMatrix` with D3D clip conventions (+Y up). The engine's projection
  bakes a Vulkan-style Y flip, so `NrdDenoiser` negates row 1 before the handoff. Getting this
  wrong is invisible for static views and yaw motion and breaks pitch/roll/vertical motion (the
  pitch gate exists for exactly this).
* viewZ is signed view-space z (negative for visible geometry); sky uses a sentinel beyond
  `denoisingRange`.
* Skipped-lobe frames write hit distance EXACTLY 0 (`HitDistanceReconstructionMode` fills them);
  any positive floor collapses ReBLUR's blur radius.
* Sky pixels write a valid placeholder normal: ReBLUR's temporal accumulation averages neighbor
  normals without range gating, so NaN corrupts adjacent geometry.

## Display pipeline: handoff and freeze

ReBLUR's capped history re-blends fresh noise forever, so a static view would shimmer. The resolve
cross-fades to the progressive accumulator over `[HandoffStartSamples, HandoffEndSamples]`
(scaled down to max_spp when smaller, skipped entirely when max_spp is below the window so the
max_spp=1 motion harnesses keep filming ReBLUR output). An output-side EMA suppresses residual
pops. When a frame's samples complete the max_spp target, that final resolve runs with weight 1
and EMA 0: the frozen frame equals the accumulator bit-exactly (asserted by the test suite).
The RNG seed advances every dispatch, never resets on accumulator clears (seed pinning defeats
temporal denoising), and restarts once when the scene finishes loading so headless captures are
bit-reproducible.

## Tests

`python3 tests/nrd/run_nrd_gates.py` is the one-command suite (CI runs it with
`--allow_unsupported`): converged flicker, firefly, motion noise (yaw + pitch arms). The probe run
requires the app's `effective pipeline: gpu` log marker — on GPUs without hardware ray tracing the
app silently falls back to forward rendering and still exits 0, so exit codes alone prove nothing.
Motion gates assert all captures are pairwise distinct (a frozen/black capture sequence otherwise
passes statistical thresholds). `--test_case nrd_probe` checks that every ReBLUR pipeline compiles
to a PSO through the production MetalNrdBackend path.

## Platform support

`MetalNrdBackend` runs on macOS and iOS (`FRAMEWORK_APPLE`): the runtime consumes cooked MSL, so
spirv-cross is linked only on macOS for the `nrd_cook` test case. ReBLUR needs SIMD-group ops
(Apple A13+/M1+ GPUs) and the gpu pipeline needs the device's ray-tracing support — the existing
capability gates and `--test_case nrd_probe` cover both per device. Non-Metal RHIs return no
backend (`RHI::CreateNrdBackend` defaults to nullptr), which `NrdDenoiser` latches as a permanent
init failure.
