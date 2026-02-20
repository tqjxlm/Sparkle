# ASVGF Implementation (GPU Pipeline)

## Scope

This document describes the ASVGF implementation used by the GPU path tracer (`--pipeline gpu`) and how to validate it.

Design goals:

1. Keep legacy behavior unchanged when `--asvgf false`.
2. Provide a deterministic pass-isolation workflow for debugging.
3. Provide quantitative sanity checks for each pass.

## Pipeline Design

When `--asvgf true`, the frame path is:

```text
RayTraceNoisy+Features
  -> Reprojection+Temporal (history ping-pong)
  -> Variance
  -> A-trous (N iterations, adaptive taper in final compose)
  -> scene_texture_
  -> ToneMapping / UI / Present
```

When `--asvgf false`, the renderer uses the legacy accumulated ray-trace output path.

## Runtime API

The ASVGF runtime controls are registered in `RenderConfig`:

| Option | Type | Default | Notes |
| --- | --- | --- | --- |
| `--asvgf` | bool | `false` | Enables ASVGF in GPU pipeline only. |
| `--asvgf_atrous_iterations` | uint | `3` | Clamped to `[0, 8]`. |
| `--asvgf_history_cap` | uint | `64` | Must be `>= 1`. Effective cap also respects `max_spp`. |
| `--asvgf_test_stage` | enum | `off` | `off, raytrace, reprojection, temporal, variance, atrous_iter`. |
| `--asvgf_debug_view` | enum | `none` | `none, noisy, normal, albedo, depth, reprojection_mask, history_length, moments, variance, filtered, reprojection_reason, reprojection_uv, reprojection_depth_error, quadrant, history_length_raw`. |
| `--asvgf_freeze_history` | bool | `false` | Disables history write/update for reprojection debugging. |
| `--asvgf_force_clear_history` | bool | `false` | Forces history reset every frame. |
| `--asvgf_test_camera_nudge_yaw` | float | `0` | Test-only deterministic camera yaw nudge (degrees). |
| `--asvgf_test_camera_nudge_pitch` | float | `0` | Test-only deterministic camera pitch nudge (degrees). |
| `--asvgf_test_post_nudge_frames` | uint | `1` | Frames to wait after nudge before screenshot (`1..16`). |

## Key Files

Renderer/config:

1. `libraries/include/renderer/RenderConfig.h`
2. `libraries/source/renderer/RenderConfig.cpp`
3. `libraries/include/renderer/renderer/GPURenderer.h`
4. `libraries/source/renderer/renderer/GPURenderer.cpp`
5. `libraries/include/renderer/renderer/ASVGF.h`
6. `libraries/source/renderer/renderer/ASVGF.cpp`

Shaders:

1. `shaders/ray_trace/ray_trace.cs.slang`
2. `shaders/ray_trace/asvgf_reprojection.cs.slang`
3. `shaders/ray_trace/asvgf_variance.cs.slang`
4. `shaders/ray_trace/asvgf_atrous.cs.slang`
5. `shaders/ray_trace/asvgf_debug_visualize.cs.slang`

Tests:

1. `dev/asvgf_sanity_test.py`
2. `dev/functional_test.py`

## Pass Contracts

### 1) RayTraceNoisy + Features

Inputs:

1. Scene TLAS/material/bindless data.
2. Camera data and frame sample settings.

Outputs:

1. `noisyImage` (`RGBAFloat`) per-frame noisy radiance.
2. Feature textures:
   1. normal+roughness (`RGBAFloat16`)
   2. albedo+metallic (`RGBAFloat16`)
   3. depth (`R32_FLOAT`)
   4. primitive id (`R32_UINT`)

Expected quality:

1. Feature views are non-degenerate and semantically correct.
2. No NaN/INF-like corruption.

### 2) Reprojection + Temporal

Inputs:

1. Current noisy/features.
2. Previous history color/moments/features (ping-pong read).
3. Previous camera plane vectors/position.

Outputs:

1. Updated history color/moments/features (ping-pong write).
2. Reprojection mask and reprojection debug texture.
3. Temporal output to `scene_texture_`.

Expected quality:

1. Stable regions keep high reprojection validity.
2. Camera motion causes partial invalidation, not total history collapse.
3. History length grows in static regions and resets where reprojection fails.

### 3) Variance

Inputs:

1. Moments/history length.
2. Normal/depth guidance.

Outputs:

1. Variance texture (`R32_FLOAT`) for A-trous guidance.

Expected quality:

1. Non-negative and bounded.
2. Correlates with noisy high-frequency regions.
3. Trends down with longer history.

### 4) A-trous

Inputs:

1. Temporal color.
2. Variance.
3. Normal/albedo/depth/primitive-id guidance.

Outputs:

1. Filtered color (ping-pong for intermediate iterations, final write to `scene_texture_`).

Expected quality:

1. Residual noise decreases with iterations.
2. Strong geometric/material edges remain stable.
3. No right/bottom border spikes or white-edge artifacts.

### 5) Debug Visualization

Inputs:

1. Stage/view selectors plus ASVGF intermediate textures.

Outputs:

1. `asvgf_debug_texture_` used by dedicated tone-mapping pass when `debug_view != none`.

Expected quality:

1. Deterministic, inspectable per-pass views.
2. `freeze_history` and `force_clear_history` overlays are visible.

## Adaptive Behavior

In final compose (`asvgf_test_stage=off`), A-trous iterations are tapered with convergence:

1. Base iteration cap is clamped to 2.
2. At accumulated SPP >= 128, cap drops to 1.
3. At accumulated SPP >= 512, cap drops to 0.

This reduces high-frequency detail loss near convergence.

## Performance Visibility

When ASVGF is enabled, `GPURenderer::MeasurePerformance()` publishes:

1. `SPP` on-screen average.
2. `ASVGFPerf` as estimated non-ray-trace overhead relative to ray-trace pass timing.

## Test Suites

Regression (ASVGF off):

```bash
python .\dev\functional_test.py --framework glfw --pipeline gpu
```

ASVGF pass sanity (quantitative):

```bash
python .\dev\asvgf_sanity_test.py --framework glfw --suite all
```

Main suites:

1. `raytrace`
2. `reprojection`
3. `temporal`
4. `variance`
5. `atrous`
6. `compose`

Each suite captures screenshots and checks numeric invariants (noise ratios, seam checks, history behavior, variance correlation, edge preservation, compose stability).

## Debug Commands

Use pass-isolation and debug-view controls to inspect each stage:

```bash
# Raytrace features
python .\build.py --framework glfw --run --pipeline gpu -- --asvgf true --asvgf_test_stage raytrace --asvgf_debug_view normal --spp 1 --max_spp 1

# Reprojection mask
python .\build.py --framework glfw --run --pipeline gpu -- --asvgf true --asvgf_test_stage reprojection --asvgf_debug_view reprojection_mask --spp 1 --max_spp 64

# Temporal history
python .\build.py --framework glfw --run --pipeline gpu -- --asvgf true --asvgf_test_stage temporal --asvgf_debug_view history_length --spp 1 --max_spp 64

# Variance
python .\build.py --framework glfw --run --pipeline gpu -- --asvgf true --asvgf_test_stage variance --asvgf_debug_view variance --spp 1 --max_spp 64

# A-trous (example iteration count)
python .\build.py --framework glfw --run --pipeline gpu -- --asvgf true --asvgf_test_stage atrous_iter --asvgf_debug_view filtered --asvgf_atrous_iterations 3 --spp 1 --max_spp 64
```

Quantitative sanity checks:

```bash
python .\dev\asvgf_sanity_test.py --framework glfw --suite reprojection
python .\dev\asvgf_sanity_test.py --framework glfw --suite all
```

## CI and Regression Policy

1. Ground-truth screenshot comparison in `dev/functional_test.py` is the regression gate for ASVGF-off behavior.
2. ASVGF-on validation should use `dev/asvgf_sanity_test.py` unless dedicated ASVGF-on baselines are introduced.
3. Recommended pre-merge checks for ASVGF work:
   1. `python .\dev\functional_test.py --framework glfw --pipeline gpu`
   2. `python .\dev\asvgf_sanity_test.py --framework glfw --suite all`

## Known Limitations

1. Current reprojection depends on camera motion and primitive/depth/normal checks; explicit per-object motion vectors are not implemented.
2. Ground-truth image comparison in CI remains valid for ASVGF-off only unless dedicated ASVGF baselines are added.
3. ASVGF sanity metrics are screenshot-space checks and can be tone-map sensitive; optional tonemap-sensitive checks exist but are not default gates.
