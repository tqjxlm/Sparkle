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
# Build
python build.py --framework glfw

# Run with REBLUR enabled (1 spp, accumulate up to 2048 frames)
python build.py --framework glfw --run --pipeline gpu --use_reblur true --spp 1 --max_spp 2048

# Run with camera animation
python build.py --framework glfw --run --pipeline gpu --use_reblur true --spp 1 \
  --camera_animation orbit_sweep --camera_animation_frames 60
```

When `--use_reblur true` is set, the renderer switches from the vanilla combined-output path tracer to a split-output path tracer that feeds separate diffuse/specular channels into a 7-stage denoising pipeline, followed by remodulation compositing and standard tone mapping.

---

## Command-Line Usage

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--use_reblur` | bool | `false` | Enable the REBLUR denoiser pipeline |
| `--reblur_debug_pass` | uint | `99` | Debug output stage selector (see [Debugging](#debugging)) |
| `--camera_animation` | string | `none` | Camera motion path: `none`, `orbit_sweep`, `dolly` |
| `--camera_animation_frames` | uint | `0` | Duration of camera animation in frames (`0` = `max_spp / 2`) |
| `--clear_screenshots` | bool | `false` | Delete stale screenshots before a test run |
| `--test_timeout` | uint | `0` | Timeout in seconds for automated tests (`0` = no timeout) |

**Constraints:**
- `--use_reblur` requires `--pipeline gpu`
- `--spp 1` is recommended when REBLUR is enabled (the denoiser replaces spatial averaging)
- `--camera_animation` is independent of `--use_reblur` but is most useful with it

---

## Tunable Parameters

### ReblurSettings (C++)

All denoiser behavior is controlled through `ReblurSettings`, passed to `ReblurDenoiser::Denoise()` each frame.

#### Blur Radii

| Parameter | Default | Description |
|-----------|---------|-------------|
| `max_blur_radius` | `30.0` | Maximum spatial blur radius in pixels for the Blur pass |
| `min_blur_radius` | `1.0` | Minimum blur radius clamp (prevents zero-radius at high accumulation) |
| `diffuse_prepass_blur_radius` | `30.0` | PrePass blur radius for diffuse channel |
| `specular_prepass_blur_radius` | `50.0` | PrePass blur radius for specular channel (wider due to higher variance) |

#### Temporal Accumulation

| Parameter | Default | Description |
|-----------|---------|-------------|
| `max_accumulated_frame_num` | `63` | Maximum temporal accumulation count. Capped at 63 for float16 packing compatibility |
| `max_stabilized_frame_num` | `255` | Independent stabilization counter limit. Uses `frame_index` directly, not accumSpeed |
| `history_fix_frame_num` | `3` | Number of early frames where HistoryFix is active to fill disoccluded regions |
| `history_fix_stride` | `14.0` | Pixel stride of the 5x5 bilateral kernel used in HistoryFix |

#### Disocclusion Detection

| Parameter | Default | Description |
|-----------|---------|-------------|
| `disocclusion_threshold` | `0.01` | Relative depth difference threshold for disocclusion detection. Larger values are more permissive |
| `plane_dist_sensitivity` | `0.02` | Sensitivity of plane-distance-based geometry rejection |

#### Bilateral Filter Weights

| Parameter | Default | Description |
|-----------|---------|-------------|
| `lobe_angle_fraction` | `0.15` | Normal weight threshold: `fraction / (1 + accumSpeed)`. Tightens with accumulation |
| `roughness_fraction` | `0.15` | Roughness difference tolerance for specular bilateral filtering |
| `min_hit_dist_weight` | `0.1` | Minimum weight contribution from hit distance in bilateral kernel |

#### Hit Distance Normalization

| Parameter | Default | Description |
|-----------|---------|-------------|
| `hit_dist_params[0]` (A) | `3.0` | Constant offset for hit distance normalization scale |
| `hit_dist_params[1]` (B) | `0.1` | View-Z linear coefficient |
| `hit_dist_params[2]` (C) | `20.0` | Roughness-dependent multiplier |
| `hit_dist_params[3]` (D) | `-25.0` | Roughness-dependent exponent |

The normalization formula:
```
scale = (A + |viewZ| * B) * lerp(1, C, exp2(D * roughness^2))
normHitDist = saturate(hitDist / scale)
```

#### Temporal Stabilization

| Parameter | Default | Description |
|-----------|---------|-------------|
| `stabilization_strength` | `1.0` | Overall strength of temporal stabilization (0 disables) |
| `antilag_sigma_scale` | `2.0` | Color box sigma multiplier for anti-lag clamping |
| `antilag_sensitivity` | `3.0` | Sensitivity of anti-lag detection (higher = more aggressive reset) |
| `fast_history_sigma_scale` | `2.0` | Sigma scale for fast history clamping reference |

#### Firefly Suppression

| Parameter | Default | Description |
|-----------|---------|-------------|
| `enable_anti_firefly` | `true` | Enable temporal and spatial firefly suppression |

### Shader Constants

Compile-time constants defined in `shaders/include/reblur_config.h.slang`:

| Constant | Value | Description |
|----------|-------|-------------|
| `REBLUR_POISSON_SAMPLE_NUM` | `8` | Number of Poisson disk samples per blur pass |
| `REBLUR_TILE_SIZE` | `16` | Tile classification block size |
| `REBLUR_DENOISING_RANGE` | `1000.0` | View-Z threshold for sky classification |
| `REBLUR_PRE_BLUR_FRACTION_SCALE` | `2.0` | Normal rejection aggressiveness in PrePass |
| `REBLUR_BLUR_FRACTION_SCALE` | `1.0` | Normal rejection aggressiveness in Blur |
| `REBLUR_POST_BLUR_FRACTION_SCALE` | `0.5` | Normal rejection aggressiveness in PostBlur |
| `REBLUR_POST_BLUR_RADIUS_SCALE` | `2.0` | PostBlur radius multiplier relative to Blur |
| `REBLUR_HISTORY_FIX_FILTER_RADIUS` | `2` | Half-size of the 5x5 HistoryFix kernel |
| `REBLUR_ANTI_FIREFLY_FILTER_RADIUS` | `4` | Spatial extent of firefly suppression |
| `REBLUR_ANTI_FIREFLY_SIGMA_SCALE` | `2.0` | Luminance sigma scale for firefly detection |
| `REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY` | `38.0` | Maximum allowed relative intensity before clamping |
| `REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE` | `0.3` | Minimum scale applied to firefly-clamped samples |

---

## Design Overview

### Pipeline Architecture

REBLUR processes each frame through 7 sequential compute shader stages plus a final composite:

```
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
Tone Mapping -> Screen
```

### Diffuse/Specular Split

The path tracer outputs separate demodulated channels at the first bounce:

- **Diffuse**: `indirect_diffuse_radiance / albedo` (removes material color)
- **Specular**: `indirect_specular_radiance` (no demodulation for metals)
- **Hit distance**: Distance from primary hit to secondary hit, normalized to [0,1]

Demodulation allows the denoiser to operate in material-independent radiance space, avoiding color bleeding across different materials.

### Temporal Accumulation Strategy

Each pixel maintains an **accumulation speed** counter (0 to `max_accumulated_frame_num`):

- Incremented each frame when the pixel is successfully reprojected
- Reset to 0 on disocclusion (depth/normal discontinuity between frames)
- Blending weight: `1 / (1 + accumSpeed)` -- exponentially decreasing contribution of new samples

This provides rapid convergence on static content while immediately adapting to newly revealed surfaces.

### Self-Stabilizing Blur

The spatial blur radius is **inversely proportional** to temporal accumulation:

```
radius = maxBlurRadius * sqrt(hitDistFactor * nonLinearAccumSpeed)
```

- Early frames: large radius (aggressive denoising)
- Converged frames: small radius (preserves detail)

### Convergence and PT Blend

At high sample counts, the composite pass blends toward the path tracer's vanilla accumulated result:

```
pt_weight = saturate(frame_index / 256)
final = lerp(denoised, pt_accumulated, pt_weight)
```

This eliminates demodulation/remodulation artifacts at silhouette edges that are inherent in split-channel denoisers, while still benefiting from aggressive denoising at low sample counts.

### Texture Budget

At 1920x1080 resolution, REBLUR allocates approximately **246 MB** of GPU memory:

| Category | Textures | Format | Size |
|----------|----------|--------|------|
| Input signals (diff + spec) | 2 | RGBA16F | 32 MB |
| G-buffer (normal, viewZ, MV, albedo) | 4 | mixed | 28 MB |
| Ping-pong temporaries | 4 | RGBA16F | 64 MB |
| History buffers | 2 | RGBA16F | 32 MB |
| Stabilized history (ping-pong) | 4 | RGBA16F | 64 MB |
| Previous-frame data | 3 | mixed | 20 MB |
| Internal data + tiles | 3 | mixed | ~6 MB |
| **Total** | **~26** | | **~246 MB** |

---

## File Structure

### C++ Source

```
libraries/
  include/renderer/denoiser/
    ReblurDenoiser.h              # Public API: ReblurSettings, ReblurInputBuffers,
                                  #   ReblurMatrices, ReblurDenoiser class
  source/renderer/denoiser/
    ReblurDenoiser.cpp            # Implementation: 7-stage pipeline, texture
                                  #   management, uniform buffer updates

  include/renderer/renderer/
    GPURenderer.h                 # Integration: RenderReblurPath(), reblur_ member
  source/renderer/renderer/
    GPURenderer.cpp               # Render flow: split PT -> denoise -> composite

  include/renderer/
    RenderConfig.h                # CLI args: use_reblur, reblur_debug_pass,
                                  #   camera_animation, camera_animation_frames
  source/renderer/
    RenderConfig.cpp              # Arg parsing, validation

  include/scene/component/camera/
    CameraAnimator.h              # Programmatic camera paths: orbit_sweep, dolly
  source/scene/component/camera/
    CameraAnimator.cpp

  include/renderer/proxy/
    CameraRenderProxy.h           # Previous-frame matrix storage for reprojection
```

### Shaders

```
shaders/ray_trace/
  ray_trace_split.cs.slang              # Split-output path tracer (diff/spec/gbuffer)
  reblur_classify_tiles.cs.slang        # Stage 1: Tile classification
  reblur_blur.cs.slang                  # Stages 2/5/6: PrePass, Blur, PostBlur
  reblur_temporal_accumulation.cs.slang # Stage 3: Motion-vector reprojection
  reblur_history_fix.cs.slang           # Stage 4: Disocclusion fill
  reblur_temporal_stabilization.cs.slang# Stage 7: Anti-lag + flicker reduction
  reblur_composite.cs.slang             # Final: remodulation + PT blend

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

```
tests/reblur/
  # C++ unit tests
  ReblurSmokeTest.cpp                   # Launch + 30-frame capture
  ReblurPassValidationTest.cpp          # Per-stage crash safety (10 frames)
  ReblurTemporalConvergenceTest.cpp     # 30+ frame convergence validation
  ReblurMatrixInfraTest.cpp             # Previous-frame matrix storage
  ReblurMotionVectorTest.cpp            # MV computation under camera motion
  ReblurReprojectionTest.cpp            # Bilinear/Catmull-Rom reprojection
  ReblurStaticNonRegressionTest.cpp     # Static camera regression check

  # Python test suites
  reblur_test_suite.py                  # Master suite (19 tests)
  reblur_pass_validation.py             # Per-pass spatial validation
  reblur_temporal_validation.py         # Per-pass temporal validation
  reblur_motion_validation.py           # Camera motion quality tests
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

    // Sentinel value: run full pipeline (no debug visualization).
    static constexpr uint32_t DebugPassDisabled = 99;

    // Run the complete REBLUR denoising pipeline for one frame.
    //   inputs      - path tracer output textures
    //   settings    - tunable denoiser parameters
    //   matrices    - current and previous frame camera matrices
    //   frame_index - monotonically increasing frame counter
    //   debug_pass  - stage to visualize (0-4) or DebugPassDisabled
    void Denoise(
        const ReblurInputBuffers& inputs,
        const ReblurSettings& settings,
        const ReblurMatrices& matrices,
        uint32_t frame_index,
        uint32_t debug_pass = DebugPassDisabled
    );

    // Access denoised output after Denoise() returns.
    RHIImage* GetDenoisedDiffuse() const;
    RHIImage* GetDenoisedSpecular() const;

    // Access internal accumulation data (for debug/composite binding).
    RHIImage* GetInternalData() const;

    // Reset all history buffers (e.g., after scene change).
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
1. Dispatches the split path tracer to populate input buffers
2. Constructs `ReblurMatrices` from `CameraRenderProxy` current and previous frame data
3. Calls `reblur_->Denoise(inputs, settings, matrices, frame_index, debug_pass)`
4. Runs the composite shader: `denoised_diff * albedo + denoised_spec`
5. Passes the composited result to tone mapping

### CameraAnimator

```cpp
namespace sparkle {

class CameraAnimator {
public:
    enum class PathType : uint8_t {
        kNone,        // No animation
        kOrbitSweep,  // Orbit around scene center with yaw sweep
        kDolly        // Forward/backward dolly motion
    };

    static PathType FromString(const std::string& name);

    struct OrbitPose {
        Vector3 center;   // Orbit center point
        float radius;     // Distance from center
        float pitch;      // Elevation angle (degrees)
        float yaw;        // Azimuth angle (degrees)
    };

    void Setup(PathType type, uint32_t total_frames, const OrbitPose& initial_pose);

    // Get the camera pose for a given frame index.
    OrbitPose GetPose(uint32_t frame_index) const;

    bool IsActive() const;
    bool IsDone(uint32_t frame_index) const;
};

} // namespace sparkle
```

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
- **Bilateral weights**: plane distance + normal angle + hit distance
- Reduces initial noise entropy before temporal accumulation.

#### 3. Temporal Accumulation

- **Shader**: `reblur_temporal_accumulation.cs.slang`
- Reprojects current pixels into previous frame using motion vectors
- **History sampling**: Bilinear with per-sample occlusion validation; upgrades to 5-tap Catmull-Rom when all 4 bilinear corners are valid
- **Disocclusion detection**: depth + normal agreement tests
- Maintains per-pixel accumulation speed counter in `internalData` (RG16Float)
- **Firefly suppression**: clamps outlier luminance scaled by `1 / (1 + accumSpeed)`

#### 4. History Fix

- **Shader**: `reblur_history_fix.cs.slang`
- Active only during first `history_fix_frame_num` frames after disocclusion
- 5x5 bilateral kernel with `history_fix_stride` pixel spacing
- Fills newly disoccluded regions with plausible data from a wide neighborhood

#### 5. Blur (Primary Spatial)

- **Shader**: `reblur_blur.cs.slang` (pass_index=1)
- **Radius**: `max_blur_radius * sqrt(hitDistFactor * nonLinearAccumSpeed)`, clamped to `min_blur_radius`
- Poisson disk rotated per-pixel (deterministic at high accumSpeed >= 16)
- Bilateral weights: plane distance + normal + hit distance + roughness + Gaussian falloff
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

### Composite

- **Shader**: `reblur_composite.cs.slang`
- Remodulates: `color = denoised_diffuse * albedo + denoised_specular`
- PT blend: `final = lerp(denoised_color, pt_accumulated, frame_index / 256)`
- Sky pixels are skipped (preserved from vanilla accumulation)

### Shared Utilities

| Header | Purpose |
|--------|---------|
| `reblur_config.h.slang` | Compile-time constants (tile size, sample count, blur scales) |
| `reblur_common.h.slang` | Hit distance normalization, octahedral normal encoding/decoding, luminance, specular magic curve, roughness weight, YCoCg conversion |
| `reblur_data.h.slang` | Accumulation speed packing/unpacking for RG16Float internal data |
| `reblur_reprojection.h.slang` | `BilinearHistorySample()` with per-corner occlusion test, `CatmullRomHistorySample()` with 5-tap optimized hardware filtering |

---

## Camera Motion

REBLUR supports full camera motion through a pipeline of matrix management and motion vector computation.

### Data Flow

```
Frame N:
  CameraRenderProxy::BeginFrame()
    -> Save current matrices as "previous" (view, projection, viewProjection)

  CameraAnimator::GetPose(frame_index)
    -> Compute new camera position/orientation for this frame

  Split Path Tracer
    -> Receives worldToClip (current) and worldToClipPrev (previous)
    -> Computes per-pixel screen-space motion vectors:
       prevClip = worldToClipPrev * worldPos
       motionVector = currentUV - prevUV

  ReblurDenoiser::Denoise()
    -> TemporalAccum uses motion vectors to find history pixel
    -> BilinearHistorySample validates depth/normal continuity
    -> Disoccluded pixels reset accumulation
```

### Camera Animation Paths

| Path | Description | Motion |
|------|-------------|--------|
| `none` | No animation (static camera) | N/A |
| `orbit_sweep` | Orbits around scene center | 45-degree yaw sweep over `camera_animation_frames` |
| `dolly` | Forward/backward along view axis | 30% radius change over `camera_animation_frames` |

### ReblurMatrices for Motion

When camera moves, `ReblurMatrices` must accurately reflect the frame-to-frame transform:

- `world_to_clip_prev`: projects world positions into previous frame's screen space
- `camera_delta`: world-space position change for parallax-based disocclusion weighting
- `framerate_scale`: `dt / dt_reference` for consistent temporal feedback at variable framerates

---

## Testing

### Test Suite

Run the full test suite:

```bash
python build.py --framework glfw  # Build first
python tests/reblur/reblur_test_suite.py --framework glfw
```

The master suite (`reblur_test_suite.py`) runs 19 test cases covering:

| Category | Tests | Validates |
|----------|-------|-----------|
| Smoke | 1 | No crash, screenshot captured |
| Quality | 2 | FLIP <= 0.08 vs 2048-spp reference |
| Temporal | 4 | Convergence, stability, no NaN/Inf |
| Per-pass spatial | 4 | Each stage produces valid output |
| Per-pass temporal | 4 | Each stage maintains temporal coherence |
| Camera motion | 4 | Quality under orbit_sweep and dolly |

### Quality Targets

| Metric | Target | Description |
|--------|--------|-------------|
| FLIP score | <= 0.08 | Perceptual difference vs 2048-spp reference |
| Pixels > 0.1 FLIP | < 0.5% | Percentage of high-error pixels |
| Pixels > 0.05 FLIP | < 0.1% | Percentage of medium-error pixels |
| Frame-to-frame std | < 0.02 | Per-pixel luminance stability |
| vs vanilla ratio | < 3.0x | Denoised error relative to vanilla PT at same SPP |

### Running Individual Tests

```bash
# Smoke test (launches, captures screenshot, exits)
python build.py --framework glfw --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 30 --test_case reblur_smoke

# Quality test at 64 spp
python build.py --framework glfw --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 64 --auto_screenshot true

# Per-pass debug (output after PrePass only)
python build.py --framework glfw --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 64 --reblur_debug_pass 0

# Camera motion test
python build.py --framework glfw --run --pipeline gpu --use_reblur true \
  --spp 1 --max_spp 60 --camera_animation orbit_sweep

# Regression test (REBLUR disabled, should match baseline)
python build.py --framework glfw --run --pipeline gpu --use_reblur false \
  --spp 1 --max_spp 2048 --auto_screenshot true
```

---

## Debugging

### Debug Pass Modes

The `--reblur_debug_pass` flag controls which pipeline stage's output is visualized:

| Value | Stage | Shows |
|-------|-------|-------|
| `0` | After PrePass | Spatially pre-filtered signal (before temporal) |
| `1` | After Blur | Primary spatial blur result |
| `2` | After PostBlur | Final spatial blur (wider radius) |
| `3` | After TemporalAccum | Temporal accumulation output (before spatial) |
| `4` | After HistoryFix | Disocclusion-fixed temporal output |
| `99` | Full pipeline | Normal operation (default) |
| `255` | Passthrough | Raw path tracer output (no denoising) |

### Common Issues

**Black pixels in geometry regions**: Usually caused by disocclusion without proper HistoryFix. Check that `history_fix_frame_num > 0` and increase `history_fix_stride` if regions are large.

**Excessive blur / loss of detail**: Reduce `max_blur_radius` or increase `max_accumulated_frame_num` to converge faster. Check that hit distance normalization parameters match the scene scale.

**Fireflies (bright sparkles)**: Ensure `enable_anti_firefly` is `true`. If fireflies persist, reduce `REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY` in the shader config.

**Ghosting under camera motion**: Verify that `CameraRenderProxy` correctly stores previous-frame matrices. Check `disocclusion_threshold` -- too large allows stale history, too small causes flickering.

**Edge artifacts at convergence**: The PT blend (`frame_index / 256`) handles this automatically. If artifacts appear before frame 256, the demodulation/remodulation may have issues in `albedo_metallic` buffer values.

**Temporal instability / flickering**: Increase `stabilization_strength` or `antilag_sigma_scale`. Check that `max_stabilized_frame_num` is not set to 0 (which disables stabilization).

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
