# ASVGF Implementation Plan (GPU Renderer)

## NOTICE

* Always read CLAUDE.md for general background info.
* If you feel this file needs improving, edit it. This includes encountered pitfalls, possible improvement, change to the plan, change to test methods, change to pass criteria, etc.
* Check git log to see if there are any relevant changes to this file.
* After finishing a step, tick the checkbox in the plan below. If the step is too large, break it down into smaller testable steps.
* When testing, operate all tests from previous steps to make sure no regression is introduced.
* Use ground truth from docs/CI.md which is generated for max_spp=2048. If you want to generate ground truth locally, you should only use max_spp>2048 to make a reasonable comparison.
* The target of asvgf is to improve converging speed. You should get at least the same quality at max_spp=2048 as the baseline. Any degradation is not acceptable.
* Make sure you change is isolated, i.e. should not make any difference when asvgf is disabled.

## Goal

Implement ASVGF denoising in the GPU path tracer (`RenderConfig::Pipeline::gpu`) with minimal regression risk:

1. Keep current behavior unchanged when ASVGF is disabled.
2. Add a production path for low-SPP real-time denoising.
3. Validate each milestone with explicit pass/fail criteria.

## Current GPU Pipeline Analysis

### Frame lifecycle today

1. `Renderer::Tick()` updates scene proxies, camera, bindless resources, and TLAS.
2. `GPURenderer::Render()`:
   1. Clears `scene_texture_` when camera/scene marks pixels dirty.
   2. Dispatches `ray_trace.cs.slang` compute and writes directly to `scene_texture_` as temporally averaged HDR color.
   3. Runs `ToneMappingPass` (`scene_texture_ -> tone_mapping_output_`).
   4. Optional UI pass.
   5. Final `ScreenQuadPass` to back buffer.

### Important constraints from current code

1. GPU pipeline currently has no dedicated feature buffers (no stored normals/depth/albedo/motion/history).
2. No previous-frame camera matrices are exposed to GPU denoising passes.
3. Dynamic object motion vectors are not currently implemented (there is a TODO for movable BLAS).
4. Available pixel formats are limited (`RGBAFloat`, `RGBAFloat16`, `R32_FLOAT`, `R32_UINT`, etc.), so ASVGF packing must fit those formats.
5. Vulkan relies on explicit image transitions; Metal transition calls are no-op, so pass ordering and resource usage must remain consistent across both backends.

## Target ASVGF Pipeline (When Enabled)

1. Ray trace noisy frame + feature extraction.
2. Temporal reprojection + moments/history update.
3. Variance estimation.
4. A-trous edge-aware filtering (ping-pong).
5. Tone mapping + UI + present (unchanged downstream flow).

Suggested high-level flow:

```text
RayTraceNoisy -> TemporalAccumulation -> Variance -> A-Trous x N -> scene_texture_ -> ToneMapping -> Present
```

## Pass-Level Test Strategy (Mandatory)

To avoid difficult end-to-end debugging, each pass must be testable in isolation.

Add these debug/test controls early (S1):

1. `--asvgf_test_stage [off, raytrace, reprojection, temporal, variance, atrous_iter]`
2. `--asvgf_debug_view [none, noisy, normal, albedo, depth, reprojection_mask, history_length, moments, variance, filtered]`
3. `--asvgf_freeze_history [true|false]` for deterministic reprojection debugging.
4. `--asvgf_force_clear_history [true|false]` to verify reset behavior immediately.

### Pass Test Matrix

1. `RayTraceNoisy + Features`
   1. Input contract: scene + camera + bindless + TLAS
   2. Output contract: noisy radiance + feature buffers valid at every pixel
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage raytrace --asvgf_debug_view normal --spp 1 --max_spp 1
```

   4. Pass criterion: normal/albedo/depth debug outputs are plausible and stable; no NaN/INF pixels.

2. `Reprojection`
   1. Input contract: current features + previous history + previous camera state
   2. Output contract: valid/invalid reprojection mask + reprojected color
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage reprojection --asvgf_debug_view reprojection_mask --spp 1 --max_spp 64
```

   4. Pass criterion: static camera has mostly valid reprojection; camera move/disocclusion shows expected invalid regions.

3. `TemporalAccumulation + Moments`
   1. Input contract: noisy current sample + valid reprojection inputs
   2. Output contract: temporal color + moments + history length
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage temporal --asvgf_debug_view history_length --spp 1 --max_spp 64
```

   4. Pass criterion: history length grows in stable areas and resets on invalid reprojection; temporal noise visibly decreases.

4. `Variance`
   1. Input contract: moments/history
   2. Output contract: non-negative, bounded variance texture
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage variance --asvgf_debug_view variance --spp 1 --max_spp 64
```

   4. Pass criterion: variance highlights noisy regions and decreases as history accumulates.

5. `A-trous (each iteration)`
   1. Input contract: temporal color + variance + guidance features
   2. Output contract: iteration `k` output with reduced residual noise, preserved edges
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage atrous_iter --asvgf_debug_view filtered --asvgf_atrous_iterations 1 --spp 1 --max_spp 64
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage atrous_iter --asvgf_debug_view filtered --asvgf_atrous_iterations 3 --spp 1 --max_spp 64
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage atrous_iter --asvgf_debug_view filtered --asvgf_atrous_iterations 5 --spp 1 --max_spp 64
```

   4. Pass criterion: residual noise decreases with iteration count; major geometric/material edges remain sharp.

6. `Final Compose (A-trous output -> tone mapping -> present)`
   1. Input contract: final filtered HDR
   2. Output contract: stable LDR final image
   3. Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --asvgf_test_stage off --spp 1 --max_spp 64 --auto_screenshot true
```

   4. Pass criterion: no extra flicker/artifacts introduced in post-ASVGF composition path.

## Progress Checklist

- [ ] S0: Baseline capture and guardrails
- [ ] S1: Config + renderer scaffolding
- [ ] S2: Noisy radiance + feature buffer output
- [ ] S3: Reprojection infrastructure (previous frame state)
- [ ] S4: Temporal accumulation and moments
- [ ] S5: Variance estimation pass
- [ ] S6: A-trous edge-aware filter passes
- [ ] S7: Integration polish, tuning, and performance budget
- [ ] S8: Final validation and documentation
- [ ] P1: Pass test - RayTraceNoisy + Features
- [ ] P2: Pass test - Reprojection
- [ ] P3: Pass test - TemporalAccumulation + Moments
- [ ] P4: Pass test - Variance
- [ ] P5: Pass test - A-trous iterations (1/3/5)
- [ ] P6: Pass test - Final Compose

## Step Plan With Testable Results

### S0: Baseline capture and guardrails

Implementation:

1. Capture baseline screenshots and GPU timing with current GPU pipeline and ASVGF disabled.
2. Record baseline for `TestScene` at low-SPP and high-SPP settings for later quality comparisons.

Testable result:

1. Baseline screenshots exist in `generated/screenshots/`.
2. Baseline GPU timing values are recorded from logs.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --spp 1 --max_spp 1 --auto_screenshot true
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --spp 1 --max_spp 256 --auto_screenshot true
```

Pass criterion:

1. Both runs complete without crash.
2. Screenshots are generated.
3. Timing and screenshots are saved for comparison before any ASVGF changes.

### S1: Config + renderer scaffolding

Implementation:

1. Add render config toggles for ASVGF (minimum: enable flag, A-trous iteration count, history cap, and pass-isolation controls from the pass test strategy).
2. Add ASVGF resource members and pass dispatch skeleton to `GPURenderer` without changing output yet.
3. Keep existing code path as default/fallback when ASVGF is disabled.

Testable result:

1. Build succeeds with new config fields.
2. `--asvgf false` keeps current GPU output path unchanged.
3. `--asvgf_test_stage` and `--asvgf_debug_view` can route display to individual pass outputs.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf false --auto_screenshot true
python .\dev\functional_test.py --framework glfw --pipeline gpu
```

Pass criterion:

1. Functional test still passes with existing GPU ground truth when ASVGF is off.
2. No regressions in forward/deferred/cpu code paths from config changes.
3. Pass isolation controls produce visibly different outputs according to selected stage/view.

### S2: Noisy radiance + feature buffer output

Implementation:

1. Add or split a ray-tracing compute shader path that outputs per-frame noisy radiance (not long-term averaged output) when ASVGF is enabled.
2. Output first-hit features needed by ASVGF:
   1. world normal + roughness
   2. albedo + metallic
   3. depth (or world position) and primitive/object identity for validation
3. Add debug display mode(s) for feature buffer validation.

Testable result:

1. ASVGF path writes valid feature textures every frame.
2. Non-ASVGF path still writes the original accumulated color output.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 1 --auto_screenshot true
```

Pass criterion:

1. Visual inspection of debug outputs confirms expected normal/albedo/depth semantics.
2. No NaN/INF artifacts visible (black/white corruption, flickering blocks, or full-frame invalid values).
3. `P1` check can be completed independently before reprojection code exists.

### S3: Reprojection infrastructure (previous frame state)

Implementation:

1. Add previous-frame camera matrices/state to GPU-visible uniforms.
2. Add history textures (ping-pong) for color + moments/history length.
3. Implement clear/reset logic for all ASVGF history resources when camera/scene invalidates accumulation (`NeedClear()` path and scene changes).

Testable result:

1. History state persists frame-to-frame when camera is static.
2. History is reset on camera movement or scene structural changes.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64
```

Pass criterion:

1. Static camera: history visualization increases toward configured cap.
2. Camera move: history visualization drops/reset in affected pixels within 1 frame.
3. `P2` check can be completed independently with reprojection debug mask.

### S4: Temporal accumulation and moments

Implementation:

1. Implement temporal reprojection compute pass:
   1. reproject previous history
   2. validate with normal/depth/primitive checks
   3. clamp history contribution to neighborhood bounds to reduce ghosting
2. Compute and store luminance moments (`E[x]`, `E[x^2]`) and history length.

Testable result:

1. Temporal pass produces stable accumulation with reduced raw noise at 1 SPP.
2. Ghosting is bounded in moderate camera motion.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64 --asvgf_atrous_iterations 0
```

Pass criterion:

1. Compared to raw noisy output, temporal-only output has visibly lower variance in static regions.
2. No severe history trails in disoccluded regions (single-frame residual allowed, persistent trails are fail).
3. `P3` check can be completed independently with history/moments debug views.

### S5: Variance estimation pass

Implementation:

1. Implement variance computation from moments and local neighborhood stabilization.
2. Store per-pixel variance texture for A-trous guidance.
3. Add debug mode to visualize variance map.

Testable result:

1. Variance texture is non-negative and spatially correlated with noise.
2. Variance drops over time in stable regions.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64 --debug_mode debug
```

Pass criterion:

1. Variance visualization highlights noisy/high-frequency regions and is lower in flat converged areas.
2. No negative or exploding variance artifacts.
3. `P4` check can be completed independently before A-trous is enabled.

### S6: A-trous edge-aware filter passes

Implementation:

1. Implement A-trous compute shader and ping-pong resources.
2. Run configurable iteration count (`N`) with edge-stopping terms from normal/depth/albedo/variance.
3. Write final denoised HDR output into `scene_texture_` before tone mapping.

Testable result:

1. Denoised image preserves edges while reducing low-SPP noise.
2. Iteration count changes are visible and stable.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64 --asvgf_atrous_iterations 5 --auto_screenshot true
```

Pass criterion:

1. Denoised output is clearly cleaner than temporal-only output.
2. Edge detail (silhouettes/contact boundaries) is not significantly blurred.
3. `P5` check requires explicit validation at iteration counts 1, 3, and 5.

### S7: Integration polish, tuning, and performance budget

Implementation:

1. Tune default ASVGF parameters for quality/performance balance.
2. Ensure all image transitions/stages are correct for Vulkan and pass ordering is correct for Metal.
3. Keep dynamic SPP logic compatible with ASVGF weighting.

Testable result:

1. Stable denoising under normal camera movement.
2. Reasonable GPU cost increase versus baseline.

Test command:

```bash
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64
```

Pass criterion:

1. No synchronization artifacts (flickering, stale-frame reads, random block corruption).
2. GPU frame time increase is within agreed budget (recommended initial target: <= 30% at 720p, 1 SPP).

### S8: Final validation and documentation

Implementation:

1. Validate regression behavior with ASVGF disabled.
2. Validate ASVGF quality using local reference screenshots (high-SPP reference vs low-SPP denoised).
3. Update docs/config references for new runtime options and expected behavior.

Testable result:

1. Legacy pipeline behavior remains stable when ASVGF is off.
2. ASVGF mode is documented and reproducible with command-line config.

Test command:

```bash
python .\dev\functional_test.py --framework glfw --pipeline gpu
python .\build.py --framework glfw --run --pipeline gpu -- --scene TestScene --asvgf true --spp 1 --max_spp 64 --auto_screenshot true
```

Pass criterion:

1. Functional regression check passes for ASVGF-off behavior.
2. ASVGF-on screenshots and logs show expected denoise improvements and stable runtime.

## Recommended Initial File Touch List

1. `libraries/include/renderer/RenderConfig.h`
2. `libraries/source/renderer/RenderConfig.cpp`
3. `libraries/include/renderer/renderer/GPURenderer.h`
4. `libraries/source/renderer/renderer/GPURenderer.cpp`
5. `libraries/include/renderer/proxy/CameraRenderProxy.h`
6. `libraries/source/renderer/proxy/CameraRenderProxy.cpp`
7. `shaders/ray_trace/ray_trace.cs.slang` (or split ASVGF-specific ray trace shader)
8. New ASVGF shaders under `shaders/ray_trace/` or `shaders/screen/`:
   1. temporal pass
   2. variance pass
   3. A-trous pass

## Scope Notes and Risks

1. First implementation should target static geometry plus camera motion; full dynamic object motion vectors can be a follow-up.
2. Depth-of-field and strong stochastic effects may need stricter reprojection rejection to avoid ghosting.
3. Because this feature intentionally changes image output, CI ground-truth comparisons are valid only for ASVGF-off regression checks unless ground truth is updated.
