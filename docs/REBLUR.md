# REBLUR Denoiser

REBLUR (Recurrent Blur) is a temporal-spatial denoiser based on NVIDIA NRD's algorithm. It operates on separate diffuse and specular channels with normalized hit distances driving adaptive blur radii, enabling high-quality denoised output from just 1 sample per pixel per frame.

## Table of Contents

- [Quick Start](#quick-start)
- [Command-Line Usage](#command-line-usage)
- [Tunable Parameters](#tunable-parameters)
- [Design Overview](#design-overview)
- [File Structure](#file-structure)
- [C++ API Reference](#c-api-reference)
- [Shader Architecture](#shader-architecture)
- [Camera Motion](#camera-motion)
- [Testing](#testing)
- [Debugging](#debugging)

---

## Quick Start

```bash
# Build (replace <FRAMEWORK> with the platform-appropriate value — see table below)
python3 build.py --framework <FRAMEWORK>

# Run with REBLUR enabled (1 spp, accumulate up to 2048 frames)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true --spp 1 --max_spp 2048

# Run with pure denoised output (no PT blend ramp)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true --spp 1 \
  --max_spp 2048 --reblur_no_pt_blend true
```

REBLUR requires `--pipeline gpu` which needs hardware ray tracing support. The `--framework` value must match your platform:

| Platform | Framework | Notes                                                                           |
| -------- | --------- | ------------------------------------------------------------------------------- |
| macOS    | `macos`   | GLFW/MoltenVK does **not** support HW ray tracing on macOS — always use `macos` |
| Windows  | `glfw`    |                                                                                 |
| iOS      | `ios`     |                                                                                 |
| Android  | `android` |                                                                                 |

When `--use_reblur true` is set, the renderer switches from the vanilla combined-output path tracer to a split-output path tracer that feeds separate diffuse/specular channels into a 7-stage denoising pipeline, followed by remodulation compositing, an optional displayed-color history reprojection, and standard tone mapping.

---

## Command-Line Usage

| Flag                   | Type   | Default | Description                                                       |
| ---------------------- | ------ | ------- | ----------------------------------------------------------------- |
| `--use_reblur`         | bool   | `false` | Enable the REBLUR denoiser pipeline                               |
| `--reblur_debug_pass`  | string | `Full`  | Debug output stage selector (see [Debugging](#debugging))         |
| `--reblur_no_pt_blend` | bool   | `false` | Force composite to output pure denoised result (no PT blend ramp) |
| `--clear_screenshots`  | bool   | `false` | Delete stale screenshots before a test run                        |
| `--test_timeout`       | uint   | `0`     | Timeout in seconds for automated tests (`0` = no timeout)         |

**Constraints:**

- `--use_reblur` requires `--pipeline gpu`, which requires a framework with hardware ray tracing support (see platform table in [Quick Start](#quick-start))
- `--spp 1` is recommended when REBLUR is enabled (the denoiser replaces spatial averaging)

---

## Tunable Parameters

### ReblurSettings (C++)

All denoiser behavior is controlled through `ReblurSettings`, passed to `ReblurDenoiser::Denoise()` each frame.

#### Blur Radii

| Parameter                      | Default | Description                                                             |
| ------------------------------ | ------- | ----------------------------------------------------------------------- |
| `max_blur_radius`              | `30.0`  | Maximum spatial blur radius in pixels for the Blur pass                 |
| `min_blur_radius`              | `1.0`   | Minimum blur radius clamp (prevents zero-radius at high accumulation)   |
| `diffuse_prepass_blur_radius`  | `30.0`  | PrePass blur radius for diffuse channel                                 |
| `specular_prepass_blur_radius` | `50.0`  | PrePass blur radius for specular channel (wider due to higher variance) |

#### Temporal Accumulation

| Parameter                   | Default | Description                                                                          |
| --------------------------- | ------- | ------------------------------------------------------------------------------------ |
| `max_accumulated_frame_num` | `511`   | Maximum temporal accumulation count. Stored in `RG16F` internal data, so integer counts remain exact well past the default |
| `max_stabilized_frame_num`  | `255`   | Independent per-pixel stabilization counter limit, persisted in stabilized history alpha instead of being capped by TA accumSpeed |
| `history_fix_frame_num`     | `3`     | Number of early frames where HistoryFix is active to fill disoccluded regions        |
| `history_fix_stride`        | `14.0`  | Pixel stride of the 5x5 bilateral kernel used in HistoryFix                          |

#### Disocclusion Detection

| Parameter                | Default | Description                                                                                       |
| ------------------------ | ------- | ------------------------------------------------------------------------------------------------- |
| `disocclusion_threshold` | `0.01`  | Relative depth difference threshold for disocclusion detection. Larger values are more permissive |
| `plane_dist_sensitivity` | `0.02`  | Sensitivity of plane-distance-based geometry rejection                                            |

#### Bilateral Filter Weights

| Parameter             | Default | Description                                                                        |
| --------------------- | ------- | ---------------------------------------------------------------------------------- |
| `lobe_angle_fraction` | `0.15`  | Normal weight threshold: `fraction / (1 + accumSpeed)`. Tightens with accumulation |
| `roughness_fraction`  | `0.15`  | Roughness difference tolerance for specular bilateral filtering                    |
| `min_hit_dist_weight` | `0.1`   | Minimum weight contribution from hit distance in bilateral kernel                  |

#### Hit Distance Normalization

| Parameter                | Default | Description                                          |
| ------------------------ | ------- | ---------------------------------------------------- |
| `hit_dist_params[0]` (A) | `3.0`   | Constant offset for hit distance normalization scale |
| `hit_dist_params[1]` (B) | `0.1`   | View-Z linear coefficient                            |
| `hit_dist_params[2]` (C) | `20.0`  | Roughness-dependent multiplier                       |
| `hit_dist_params[3]` (D) | `-25.0` | Roughness-dependent exponent                         |

The normalization formula:

```text
scale = (A + |viewZ| * B) * lerp(1, C, exp2(D * roughness^2))
normHitDist = saturate(hitDist / scale)
```

#### Temporal Stabilization

| Parameter                  | Default | Description                                                        |
| -------------------------- | ------- | ------------------------------------------------------------------ |
| `stabilization_strength`   | `1.0`   | Overall strength of temporal stabilization (0 disables)            |
| `antilag_sigma_scale`      | `2.0`   | Color box sigma multiplier for anti-lag clamping                   |
| `antilag_sensitivity`      | `3.0`   | Sensitivity of anti-lag detection (higher = more aggressive reset) |
| `fast_history_sigma_scale` | `2.0`   | Sigma scale for fast history clamping reference                    |

#### Firefly Suppression

| Parameter             | Default | Description                                     |
| --------------------- | ------- | ----------------------------------------------- |
| `enable_anti_firefly` | `true`  | Enable temporal and spatial firefly suppression |

### Shader Constants

Compile-time constants defined in `shaders/include/reblur_config.h.slang`:

| Constant                                           | Value    | Description                                        |
| -------------------------------------------------- | -------- | -------------------------------------------------- |
| `REBLUR_POISSON_SAMPLE_NUM`                        | `8`      | Number of Poisson disk samples per blur pass       |
| `REBLUR_TILE_SIZE`                                 | `16`     | Tile classification block size                     |
| `REBLUR_DENOISING_RANGE`                           | `1000.0` | View-Z threshold for sky classification            |
| `REBLUR_PRE_BLUR_FRACTION_SCALE`                   | `2.0`    | Normal rejection aggressiveness in PrePass         |
| `REBLUR_BLUR_FRACTION_SCALE`                       | `1.0`    | Normal rejection aggressiveness in Blur            |
| `REBLUR_POST_BLUR_FRACTION_SCALE`                  | `0.5`    | Normal rejection aggressiveness in PostBlur        |
| `REBLUR_POST_BLUR_RADIUS_SCALE`                    | `2.0`    | PostBlur radius multiplier relative to Blur        |
| `REBLUR_HISTORY_FIX_FILTER_RADIUS`                 | `2`      | Half-size of the 5x5 HistoryFix kernel             |
| `REBLUR_ANTI_FIREFLY_FILTER_RADIUS`                | `4`      | Spatial extent of firefly suppression              |
| `REBLUR_ANTI_FIREFLY_SIGMA_SCALE`                  | `2.0`    | Luminance sigma scale for firefly detection        |
| `REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY` | `38.0`   | Maximum allowed relative intensity before clamping |
| `REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE`     | `0.3`    | Minimum scale applied to firefly-clamped samples   |

---

## Design Overview

### Pipeline Architecture

REBLUR processes each frame through 7 sequential compute shader stages plus a final composite. When PT blend is enabled, the renderer also reprojects the previous displayed color after composite:

```text
Split Path Tracer (1 spp)
  |
  v
+---------------------------------------------------------+
| REBLUR Pipeline                                         |
|                                                         |
|  1. ClassifyTiles    -- 16x16 tile sky/geometry flags   |
|  2. PrePass          -- 8-tap Poisson bilateral blur    |
|  3. TemporalAccum    -- MV-based history reprojection   |
|  4. HistoryFix       -- Fill disoccluded regions        |
|  5. Blur             -- Adaptive spatial blur           |
|  6. PostBlur         -- Wider blur + history write      |
|  7. TemporalStab     -- Anti-lag + flicker reduction    |
|                                                         |
+---------------------------------------------------------+
  |
  v
Composite (remodulation + PT blend)
  |
  v
FinalHistory (displayed-color reprojection)
  |
  v
Tone Mapping -> Screen
```

### Diffuse/Specular Split

The path tracer outputs separate demodulated channels at the first bounce:

- **Diffuse**: `indirect_diffuse_radiance / albedo` (removes material color)
- **Specular**: `indirect_specular_radiance` (no demodulation for metals)
- **Hit distance**: Distance from primary hit to secondary hit, normalized to [0,1]
- **Instance ID**: `CommittedInstanceIndex() % 1024` stored in `normalRoughness.a` for cross-object edge-stopping

Demodulation allows the denoiser to operate in material-independent radiance space, avoiding color bleeding across different materials.

### Temporal Accumulation Strategy

Each pixel maintains an **accumulation speed** counter (0 to `max_accumulated_frame_num`):

- Incremented each frame when the pixel is successfully reprojected
- Reset to 0 on disocclusion (depth/normal/instance_id discontinuity between frames)
- Blending weight: `1 / (1 + accumSpeed)` -- exponentially decreasing contribution of new samples
- On disocclusion, history is initialized to the current sample (not zero) to preserve energy

This provides rapid convergence on static content while immediately adapting to newly revealed surfaces.

### Self-Stabilizing Blur

The spatial blur radius is **inversely proportional** to temporal accumulation:

```text
radius = maxBlurRadius * sqrt(hitDistFactor * nonLinearAccumSpeed)
```

- Early frames: large radius (aggressive denoising)
- Converged frames: small radius (preserves detail)

### Convergence and PT Blend

At high sample counts, the composite pass blends toward the path tracer's vanilla accumulated result:

```text
pt_weight = saturate(cumulated_sample_count / 256)
final = lerp(denoised, pt_accumulated, pt_weight)
```

This eliminates demodulation/remodulation artifacts at silhouette edges that are inherent in split-channel denoisers, while still benefiting from aggressive denoising at low sample counts. After camera motion, `cumulated_sample_count` resets to 0, so the denoiser takes over during re-convergence.

The PT accumulation uses a separate `pt_accumulation_` texture (not the scene texture) to avoid contaminating the PT's temporal history with denoiser output. Use `--reblur_no_pt_blend true` to force pure denoised output for diagnostic purposes.

When PT blend is enabled for the `Full` output, `GPURenderer` also keeps a renderer-owned history of the displayed linear color. That history is reprojected with motion vectors plus previous `viewZ` / `normalRoughness`, and blended by the current REBLUR accumulation speed. This preserves converged full-pipeline brightness across small camera nudges even though the vanilla PT accumulation itself must reset on camera motion.

### Texture Budget

At 1920x1080 resolution, REBLUR allocates approximately **246 MB** of GPU memory:

| Category                             | Textures | Format  | Size        |
| ------------------------------------ | -------- | ------- | ----------- |
| Input signals (diff + spec)          | 2        | RGBA16F | 32 MB       |
| G-buffer (normal, viewZ, MV, albedo) | 4        | mixed   | 28 MB       |
| Ping-pong temporaries                | 4        | RGBA16F | 64 MB       |
| History buffers                      | 2        | RGBA16F | 32 MB       |
| Stabilized history (ping-pong)       | 4        | RGBA32F | 128 MB      |
| Previous-frame data                  | 3        | mixed   | 20 MB       |
| Internal data + tiles                | 3        | mixed   | ~6 MB       |
| **Total**                            | **~26**  |         | **~246 MB** |

---

## File Structure

### C++ Source

```text
libraries/
  include/renderer/denoiser/
    ReblurDenoiser.h              # Public API: ReblurSettings, ReblurInputBuffers,
                                  #   ReblurMatrices, ReblurDenoiser class
  source/renderer/denoiser/
    ReblurDenoiser.cpp            # Implementation: 7-stage pipeline, texture
                                  #   management, uniform buffer updates

  include/renderer/renderer/
    GPURenderer.h                 # Integration: RenderReblurPath(), reblur_ member,
                                  #   pt_accumulation_ texture
  source/renderer/renderer/
    GPURenderer.cpp               # Render flow: split PT -> denoise -> composite
                                  #   -> final displayed-color history

  include/renderer/
    RenderConfig.h                # CLI args: use_reblur, reblur_debug_pass,
                                  #   reblur_no_pt_blend, ReblurDebugPass enum
  source/renderer/
    RenderConfig.cpp              # Arg parsing, validation

  include/renderer/proxy/
    CameraRenderProxy.h           # Previous-frame matrix storage for reprojection
```

### Shaders

```text
shaders/ray_trace/
  ray_trace_split.cs.slang              # Split-output path tracer (diff/spec/gbuffer)
  reblur_classify_tiles.cs.slang        # Stage 1: Tile classification
  reblur_blur.cs.slang                  # Stages 2/5/6: PrePass, Blur, PostBlur
  reblur_temporal_accumulation.cs.slang # Stage 3: Motion-vector reprojection
  reblur_history_fix.cs.slang           # Stage 4: Disocclusion fill
  reblur_temporal_stabilization.cs.slang# Stage 7: Anti-lag + flicker reduction
  reblur_composite.cs.slang             # Final: remodulation + PT blend
  reblur_final_history.cs.slang         # Renderer-owned displayed-color reprojection

shaders/include/
  reblur_config.h.slang                 # Compile-time constants
  reblur_common.h.slang                 # Utilities: hit distance normalization,
                                        #   octahedral normal encoding, luminance,
                                        #   specular magic curve, YCoCg conversion
  reblur_data.h.slang                   # Accumulation speed packing (RG16Float)
  reblur_reprojection.h.slang           # Bilinear + Catmull-Rom history sampling
                                        #   with per-sample occlusion validation
```

### Tests

```text
tests/reblur/
  # C++ test cases
  ReblurSmokeTest.cpp                   # Launch + 30-frame capture
  ReblurTemporalConvergenceTest.cpp     # 30+ frame convergence validation
  ReblurMatrixInfraTest.cpp             # Previous-frame matrix storage
  ReblurMotionVectorTest.cpp            # MV computation under camera motion
  ReblurReprojectionTest.cpp            # Bilinear/Catmull-Rom reprojection
  ReblurStaticNonRegressionTest.cpp     # Static camera regression check
  ReblurGhostingTest.cpp                # Camera nudge ghosting detection
  ReblurConvergedHistoryTest.cpp        # History preservation after camera delta
  VanillaConvergedBaselineTest.cpp      # Vanilla baseline for comparison
  MotionLuminanceTrackTest.cpp          # Luminance tracking during motion

  # Python test suites
  reblur_test_suite.py                  # Master suite (26 tests)
  reblur_pass_validation.py             # Per-pass spatial validation
  reblur_temporal_validation.py         # Per-pass temporal validation
  reblur_motion_validation.py           # Camera motion quality tests
  test_converged_history.py             # History preservation after camera nudge
  test_motion_side_history.py           # Motion-side shell regression after camera nudge
  test_run1_semantic_e2e.py             # Semantic Run 1 shell regression on e2e output
  test_denoiser_history.py              # Pure denoiser quality after nudge
  test_denoised_motion_luma.py          # Luminance stability during motion
  test_material_id_ghosting.py          # Cross-object ghosting validation
  test_motion_vectors.py                # MV statistical validation
  test_reprojection.py                  # Reprojection output validation
```

---

## C++ API Reference

### ReblurDenoiser

```cpp
namespace sparkle {

class ReblurDenoiser {
public:
    // Construct with RHI context and render resolution.
    // Allocates all internal textures and creates compute pipelines.
    ReblurDenoiser(RHIContext* rhi, uint32_t width, uint32_t height);
    ~ReblurDenoiser();

    // Run the complete REBLUR denoising pipeline for one frame.
    //   inputs      - path tracer output textures
    //   settings    - tunable denoiser parameters
    //   matrices    - current and previous frame camera matrices
    //   frame_index - monotonically increasing frame counter
    //   debug_pass  - ReblurDebugPass enum value
    void Denoise(
        const ReblurInputBuffers& inputs,
        const ReblurSettings& settings,
        const ReblurMatrices& matrices,
        uint32_t frame_index,
        RenderConfig::ReblurDebugPass debug_pass = RenderConfig::ReblurDebugPass::Full
    );

    // Access denoised output after Denoise() returns.
    RHIImage* GetDenoisedDiffuse() const;
    RHIImage* GetDenoisedSpecular() const;

    // Access internal accumulation data (for debug/composite binding).
    RHIImage* GetInternalData() const;

    // Reset all history buffers (e.g., after scene change or scene load).
    void Reset();
};

} // namespace sparkle
```

### ReblurInputBuffers

```cpp
struct ReblurInputBuffers {
    RHIImage* diffuse_radiance_hit_dist;  // RGBA16F: demodulated diffuse + norm hit dist
    RHIImage* specular_radiance_hit_dist; // RGBA16F: specular radiance + norm hit dist
    RHIImage* normal_roughness;           // RGBA16U: octahedral normal (rg) + roughness (b)
                                          //          + instance_id (a) for cross-object edge-stopping
    RHIImage* view_z;                     // R32F:    linear view-space depth
    RHIImage* motion_vectors;             // RG16F:   screen-space motion vectors
    RHIImage* albedo_metallic;            // RGBA8U:  base color (rgb) + metallic (a)
};
```

### ReblurMatrices

```cpp
struct ReblurMatrices {
    Mat4 view_to_clip;          // Current frame: view -> clip space
    Mat4 view_to_world;         // Current frame: view -> world space
    Mat4 world_to_clip_prev;    // Previous frame: world -> clip space (for reprojection)
    Mat4 world_to_view_prev;    // Previous frame: world -> view space
    Mat4 world_prev_to_world;   // World-space delta between frames (identity if static)
    Vector3 camera_delta;       // Camera position change (world space)
    float framerate_scale;      // dt/dt_ref for framerate-independent temporal feedback
};
```

### GPURenderer Integration

```cpp
// In GPURenderer::Render():
if (config_.use_reblur) {
    RenderReblurPath();  // Split PT -> Denoise -> Composite -> Tone map
} else {
    RenderVanillaPath(); // Combined PT -> Tone map
}
```

The `RenderReblurPath()` method:

1. Dispatches the split path tracer to populate input buffers (including instance_id in `normalRoughness.a`)
2. Constructs `ReblurMatrices` from `CameraRenderProxy` current and previous frame data
3. Calls `reblur_->Denoise(inputs, settings, matrices, frame_index, debug_pass)`
4. Runs the composite shader: `denoised_diff * albedo + denoised_spec`, blending with PT accumulated result
5. For `Full` output with PT blend enabled, reprojects the previous displayed linear color and writes a stabilized final-history result
6. Passes the composited result to tone mapping

On first scene load, `GPURenderer` calls `reblur_->Reset()` to clear any history accumulated during pre-scene-load frames.

---

## Shader Architecture

### Stage Details

#### 1. ClassifyTiles

- **Shader**: `reblur_classify_tiles.cs.slang`
- **Workgroup**: 16x16
- **Output**: R8 tile map (1 texel per 16x16 block)
- Marks tiles as sky (`1.0`) when all pixels have `viewZ > REBLUR_DENOISING_RANGE`, or geometry (`0.0`). All subsequent stages early-out on sky tiles.

#### 2. PrePass (Spatial Pre-Filter)

- **Shader**: `reblur_blur.cs.slang` (pass_index=0)
- **Samples**: 8-tap Poisson disk with golden-angle rotation
- **Radius**: `diffuse_prepass_blur_radius` (30px) / `specular_prepass_blur_radius` (50px)
- **Bilateral weights**: plane distance + normal angle + hit distance + instance_id
- Reduces initial noise entropy before temporal accumulation.

#### 3. Temporal Accumulation

- **Shader**: `reblur_temporal_accumulation.cs.slang`
- Reprojects current pixels into previous frame using motion vectors
- **History sampling**: Bilinear with per-sample occlusion validation (depth + normal + instance_id); upgrades to 5-tap Catmull-Rom when all 4 bilinear corners are valid and a 3x3 instance_id neighborhood check passes
- **Disocclusion detection**: depth + normal + instance_id agreement tests
- **Disocclusion handling**: history initialized to current sample (not zero) to preserve energy
- Maintains per-pixel accumulation speed counter in `internalData` (RG16Float). Raw accum speeds (0-63) are stored, NOT normalized to [0,1]
- **Specular parallax rejection**: for smooth surfaces with camera motion (MV > 1.5px), reduces specular accumulation speed proportional to motion magnitude and inversely proportional to roughness. The 1.5px threshold avoids false triggers from sub-pixel TAA jitter (~0.7px)
- **Firefly suppression**: only active at `accumSpeed >= 16`, uses 4x relative luminance clamp. At low accumulation, the history reference is too noisy for reliable clamping

#### 4. History Fix

- **Shader**: `reblur_history_fix.cs.slang`
- Active only during first `history_fix_frame_num` frames after disocclusion
- 5x5 bilateral kernel with `history_fix_stride` pixel spacing + instance_id edge-stopping
- Fills newly disoccluded regions with plausible data from a wide neighborhood
- Anti-firefly in HistoryFix guarded by `min_accum >= 16.0` (same rationale as TA)

#### 5. Blur (Primary Spatial)

- **Shader**: `reblur_blur.cs.slang` (pass_index=1)
- **Radius**: `max_blur_radius * sqrt(hitDistFactor * nonLinearAccumSpeed)`, clamped to `min_blur_radius`
- Poisson disk rotated per-pixel (deterministic at high accumSpeed >= 16)
- Bilateral weights: plane distance + normal + hit distance + roughness + instance_id + Gaussian falloff
- Core denoising pass. Self-stabilizing: radius shrinks as history accumulates.

#### 6. PostBlur (Final Spatial)

- **Shader**: `reblur_blur.cs.slang` (pass_index=2)
- Same kernel as Blur but `RADIUS_SCALE = 2.0`, `FRACTION_SCALE = 0.5`
- Writes persistent history buffers for next frame's temporal accumulation

#### 7. Temporal Stabilization

- **Shader**: `reblur_temporal_stabilization.cs.slang`
- Computes local luminance mean and variance in YCoCg space
- Clamps stabilized history to color box: `mean +/- sigma * antilag_sigma_scale`
- Anti-lag detection: resets accumulation when current signal deviates significantly from slow history
- **Framerate scaling**: temporal feedback alpha is scaled by `framerate_scale` for consistent behavior across framerates
- **Disocclusion bypass**: when `accum_incoming <= 1.0`, skips stabilized history blend entirely and uses 100% PostBlur output. Prevents pulling stale stabilized history onto newly disoccluded pixels
- Uses motion-vector-based reprojection to sample previous stabilized history
- Stabilized history ping-pong uses `RGBA32F` so a long EMA horizon does not accumulate float16 quantization bias frame over frame

### Composite

- **Shader**: `reblur_composite.cs.slang`
- Remodulates: `color = denoised_diffuse * albedo + denoised_specular`
- PT blend: `final = lerp(denoised_color, pt_accumulated, frame_index / 256)`
- Sky pixels are skipped (preserved from vanilla accumulation)

### Displayed-Color History

- **Shader**: `reblur_final_history.cs.slang`
- Lives in `GPURenderer`, not `ReblurDenoiser`
- Reprojects the previous displayed linear color using current motion vectors plus previous `viewZ` / `normalRoughness`
- Uses current REBLUR accumulation speed as the displayed-history reuse weight
- Enabled only for `Full` output when PT blend is active; skipped for debug passes and `--reblur_no_pt_blend true`

### Shared Utilities

| Header                        | Purpose                                                                                                                                                      |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `reblur_config.h.slang`       | Compile-time constants (tile size, sample count, blur scales)                                                                                                |
| `reblur_common.h.slang`       | Hit distance normalization, octahedral normal encoding/decoding, luminance, specular magic curve, roughness weight, YCoCg conversion                         |
| `reblur_data.h.slang`         | Accumulation speed packing/unpacking for RG16Float internal data                                                                                             |
| `reblur_reprojection.h.slang` | `BilinearHistorySample()` with per-corner occlusion test (depth + normal + instance_id), `CatmullRomHistorySample()` with 5-tap optimized hardware filtering |

---

## Camera Motion

REBLUR supports full camera motion through a pipeline of matrix management, motion vector computation, and cross-object disocclusion detection.

### Data Flow

```text
Frame N:
  CameraRenderProxy::BeginFrame()
    -> Save current matrices as "previous" (view, projection, viewProjection)

  Split Path Tracer
    -> Receives worldToClip (current) and worldToClipPrev (previous)
    -> Computes per-pixel screen-space motion vectors:
       prevClip = worldToClipPrev * worldPos
       motionVector = currentUV - prevUV
    -> Stores instance_id (CommittedInstanceIndex) in normalRoughness.a

  ReblurDenoiser::Denoise()
    -> TemporalAccum uses motion vectors to find history pixel
    -> BilinearHistorySample validates depth/normal/instance_id continuity
    -> CatmullRom upgrade blocked when 3x3 neighborhood has instance_id mismatch
    -> Specular parallax rejection reduces accum speed for smooth surfaces
    -> Disoccluded pixels initialized to current sample (energy-preserving)
    -> HistoryFix, Blur, PostBlur all use instance_id edge-stopping
    -> TemporalStab bypasses stabilized history for recently disoccluded pixels
```

### Cross-Object Disocclusion

The instance_id stored in `normalRoughness.a` (`CommittedInstanceIndex() % 1024`, encoded as `float(id) / 1023.0`) enables correct disocclusion at object boundaries. Without instance_id, the denoiser can only detect disocclusion through depth/normal tests, which fail when objects have similar depth/normals (e.g., an object sliding over a floor). Instance_id checks are applied in:

1. **BilinearHistorySample** -- reject bilinear taps with different instance_id
2. **CatmullRom guard** -- fall back to bilinear when 3x3 neighborhood has mixed instance_ids
3. **Spatial blur** (PrePass, Blur, PostBlur) -- skip cross-object Poisson samples
4. **HistoryFix** -- prevent cross-object bilateral reconstruction

### ReblurMatrices for Motion

When camera moves, `ReblurMatrices` must accurately reflect the frame-to-frame transform:

- `world_to_clip_prev`: projects world positions into previous frame's screen space
- `camera_delta`: world-space position change for parallax-based disocclusion weighting
- `framerate_scale`: `dt / dt_reference` for consistent temporal feedback at variable framerates

### Camera Motion Testing

Camera motion is tested through C++ `TestCase` classes (not a standalone CameraAnimator). The `ReblurGhostingTest` warms up, nudges the camera multiple times, and captures screenshots at each step. The `ReblurConvergedHistoryTest` validates history preservation after a small camera delta.

---

## Testing

### Test Suite

Run the full test suite:

```bash
python3 build.py --framework <FRAMEWORK>  # Build first
python3 tests/reblur/reblur_test_suite.py --framework <FRAMEWORK>
```

The master suite (`reblur_test_suite.py`) runs 25 test cases covering:

| Category              | Tests | Validates                                                       |
| --------------------- | ----- | --------------------------------------------------------------- |
| Smoke                 | 1     | No crash, screenshot captured                                   |
| Quality               | 2     | FLIP vs 2048-spp reference                                      |
| Per-pass              | 4     | Each stage produces valid spatial/temporal output               |
| Temporal              | 4     | Convergence, stability, no NaN/Inf                              |
| Convergence           | 1     | Frame-to-frame instability at 2048 spp vs vanilla               |
| Motion infrastructure | 6     | Matrix storage, MV computation, reprojection, static regression |
| Ghosting              | 1     | Camera nudge cross-object ghosting detection                    |
| Camera motion quality | 1     | Temporal stability and reconvergence under motion               |
| History preservation  | 4     | Converged history, motion-side shell regression, denoiser-only quality, motion luminance |
| End-to-end            | 1     | Full pipeline FLIP vs ground truth                              |

**Latest results (2026-03-02):** 26 passed, 0 failed (697.5s total).

### Quality Targets

| Metric                             | Target    | Description                                       |
| ---------------------------------- | --------- | ------------------------------------------------- |
| FLIP score                         | <= 0.1    | Perceptual difference vs 2048-spp reference       |
| Frame-to-frame instability         | < 1.0%    | Percentage of unstable pixels at convergence      |
| vs vanilla ratio                   | < 3.0x    | Denoised error relative to vanilla PT at same SPP |
| Denoiser luma ratio (after nudge)  | 0.93-1.07 | Pure denoised output luminance preservation       |
| Denoiser noise ratio (after nudge) | < 3.0x    | Noise increase from history reprojection          |

### Running Individual Tests

```bash
# Smoke test (launches, captures screenshot, exits)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 30 --test_case reblur_smoke

# Quality test at 64 spp
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 64 --auto_screenshot true

# Per-pass debug (output after PrePass only)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 64 --reblur_debug_pass PrePass

# Ghosting test (camera nudge sequence)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 200 --test_case reblur_ghosting

# Regression test (REBLUR disabled, should match baseline)
python3 build.py --framework <FRAMEWORK> --run --pipeline gpu --use_reblur false \
  --spp 1 --max_spp 2048 --auto_screenshot true
```

---

## Debugging

### Debug Pass Modes

The `--reblur_debug_pass` flag accepts enum names and controls which pipeline stage's output is visualized:

| Name             | Shows                                                            |
| ---------------- | ---------------------------------------------------------------- |
| `Full`           | Normal operation — full pipeline (default)                       |
| `PrePass`        | Spatially pre-filtered signal (before temporal)                  |
| `Blur`           | Primary spatial blur result                                      |
| `PostBlur`       | Final spatial blur (wider radius)                                |
| `TemporalAccum`  | Temporal accumulation output (before spatial)                    |
| `HistoryFix`     | Disocclusion-fixed temporal output                               |
| `TADisocclusion` | Diagnostic: disocclusion map                                     |
| `TAMotionVector` | Diagnostic: motion vector magnitude heatmap                      |
| `TADepth`        | Diagnostic: depth visualization                                  |
| `TAHistory`      | Diagnostic: raw reprojected history                              |
| `TAMaterialId`   | Diagnostic: instance_id mismatch (R=current, G=prev, B=mismatch) |
| `Passthrough`    | Raw path tracer output (no denoising)                            |

### Common Issues

**Black pixels in geometry regions**: Usually caused by disocclusion without proper HistoryFix. Check that `history_fix_frame_num > 0` and increase `history_fix_stride` if regions are large.

**Excessive blur / loss of detail**: Reduce `max_blur_radius` or increase `max_accumulated_frame_num` to converge faster. Check that hit distance normalization parameters match the scene scale.

**Fireflies (bright sparkles)**: Ensure `enable_anti_firefly` is `true`. If fireflies persist, reduce `REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY` in the shader config.

**Cross-object ghosting**: Objects leaving radiance trails on adjacent surfaces during camera motion. Instance_id edge-stopping in BilinearHistorySample, spatial blur, HistoryFix, and CatmullRom prevents cross-object contamination. Use `--reblur_debug_pass TAMaterialId` to visualize instance_id boundaries.

**Ghosting under camera motion**: Verify that `CameraRenderProxy` correctly stores previous-frame matrices. Check `disocclusion_threshold` -- too large allows stale history, too small causes flickering.

**Edge artifacts at convergence**: The PT blend (`frame_index / 256`) handles this automatically. If artifacts appear before frame 256, the demodulation/remodulation may have issues in `albedo_metallic` buffer values.

**Temporal instability / flickering**: Increase `stabilization_strength` or `antilag_sigma_scale`. Check that `max_stabilized_frame_num` is not set to 0 (which disables stabilization).

**Energy loss after camera nudge**: If luminance drops after camera motion, check firefly suppression thresholds (should only activate at `accumSpeed >= 16`). At low accumulation, asymmetric clamping of right-skewed PT distributions causes systematic energy loss. Use `--reblur_no_pt_blend true` to isolate the denoiser from the PT blend ramp.

### Inspecting Internal State

Use `GetInternalData()` to access the RG16Float texture containing per-pixel accumulation speeds:

- R channel: diffuse accumulation speed (0 to `max_accumulated_frame_num`)
- G channel: specular accumulation speed

Low values indicate disocclusion or history rejection. Monotonically increasing values under static camera confirm correct temporal accumulation.

---

## References

- [NVIDIA NRD](https://github.com/NVIDIA-RTX/NRD) (v4.17.1)
- Ray Tracing Gems II, Chapter 49: "ReBLUR: A Hierarchical Recurrent Denoiser" (Dmitry Zhdan)
- GTC 2020: "Fast Denoising with Self-Stabilizing Recurrent Blurs"

### Local Reference Clone

A shallow clone of NVIDIA's official NRD implementation (v4.17) is available at
`/d/Projects/NRD_reference/` for comparison when debugging or extending REBLUR.
The companion math library (MathLib) is at `/d/Projects/NRD_MathLib/`.

Key files for TS comparison:
- `NRD_reference/Shaders/REBLUR_TemporalStabilization.cs.hlsl` -- Official TS shader
- `NRD_reference/Shaders/REBLUR_Common.hlsli` -- `ComputeAntilag`, `GetTemporalAccumulationParams`
- `NRD_reference/Shaders/REBLUR_Config.hlsli` -- Config defines (no `TS_MIN_SIGMA_FRACTION`)
- `NRD_reference/Include/NRDSettings.h` -- Default settings (`maxStabilizedFrameNum=63`)
- `NRD_MathLib/ml.hlsli` -- `Color::Clamp` = `clamp(value, mean-sigma, mean+sigma)`

Notable differences from our implementation documented in `docs/plans/2026-03-05-floor-noise-progress.md` (Phase 4).
