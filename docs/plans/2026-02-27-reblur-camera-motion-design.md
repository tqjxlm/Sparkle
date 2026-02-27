# REBLUR Camera Motion Design

## Overview

Add moving camera support to the REBLUR denoiser. Currently the denoiser operates under a static camera assumption (motion vectors are zero, temporal reprojection is identity). This plan implements **Surface Motion (SMB)** reprojection: screen-space motion vectors, bilinear/Catmull-Rom history sampling, parallax-based disocclusion, anti-lag, and framerate scaling.

**Virtual Motion (VMB)** for specular reflection tracking (curvature estimation, thin-lens projection) is deferred to a follow-up plan.

### References

- [NRD GitHub](https://github.com/NVIDIA-RTX/NRD) — `REBLUR_TemporalAccumulation.cs.hlsl`, `REBLUR_TemporalStabilization.cs.hlsl`, `REBLUR_Common.hlsli`
- [2026-02-25-reblur-design.md](2026-02-25-reblur-design.md) — original REBLUR design (static camera)

### Scope

| In scope | Out of scope (deferred) |
|----------|------------------------|
| Previous-frame matrix storage | Virtual Motion (VMB) / curvature estimation |
| Screen-space motion vector output | Per-object motion vectors / animated objects |
| Bilinear + Catmull-Rom history sampling | Responsive accumulation (roughness-based) |
| Parallax-based disocclusion | Material-ID-based disocclusion |
| Footprint quality weighting | |
| Anti-lag system | |
| Framerate scaling | |
| Programmatic camera paths for testing | |

### Testing Rule

Every implemented milestone MUST have dedicated test cases that verify:
1. **Semantic correctness** — logic behaves as designed (e.g., matrices update, MVs point in the right direction, reprojection hits the right pixel).
2. **Statistical properties** — numerical outputs fall within expected ranges (e.g., MV magnitude > 0 during motion, footprint quality in [0,1], no NaN/Inf).

Tests MUST be committed alongside or immediately after the feature implementation. No milestone may be marked complete without its test gate passing. Test cases use the C++ `TestCase` system for in-app validation and Python scripts for screenshot-based pixel analysis.

---

## 1. Previous-Frame Matrix Infrastructure

### Current State

`CameraRenderProxy` computes `view_matrix_`, `projection_matrix_`, and `view_projection_matrix_` each frame but does not store previous-frame values. `ReblurMatrices` is populated with identity matrices.

### Required Changes

#### CameraRenderProxy

Store previous-frame matrices before overwriting:

```cpp
// CameraRenderProxy.h additions:
Mat4 view_matrix_prev_ = Mat4::Identity();
Mat4 projection_matrix_prev_ = Mat4::Identity();
Mat4 view_projection_matrix_prev_ = Mat4::Identity();
Vec3 position_prev_ = Vec3::Zero();

// New method:
void BeginFrame();  // Call at start of each frame, before OnTransformDirty
```

```cpp
// CameraRenderProxy.cpp:
void CameraRenderProxy::BeginFrame() {
    view_matrix_prev_ = view_matrix_;
    projection_matrix_prev_ = projection_matrix_;
    view_projection_matrix_prev_ = view_projection_matrix_;
    position_prev_ = posture_.position;
}
```

#### GPURenderer: Populate ReblurMatrices

```cpp
ReblurMatrices matrices;
matrices.view_to_clip = camera->projection_matrix();
matrices.view_to_world = camera->view_matrix().inverse();
matrices.world_to_clip_prev = camera->view_projection_matrix_prev();
matrices.world_to_view_prev = camera->view_matrix_prev();
matrices.world_prev_to_world = Mat4::Identity();  // Static world, only camera moves
```

#### Camera Delta

```cpp
// ReblurDenoiser uniform:
Vec3 camera_delta = camera->position_prev() - camera->position();
```

Passed to all REBLUR shaders as `gCameraDelta`.

---

## 2. Motion Vector Computation

### In ray_trace_split.cs.slang

Replace the current `motionVectorOutput[pixel] = float2(0, 0)` with actual computation.

#### New Uniforms

```slang
float4x4 worldToClip;      // Current frame
float4x4 worldToClipPrev;  // Previous frame
```

#### Computation

At the primary hit point:

```slang
float3 worldPos = ray.Origin + ray.Direction * hitT;

// Project through current and previous frame matrices
float4 clipCurrent = mul(worldToClip, float4(worldPos, 1.0));
float2 uvCurrent = clipCurrent.xy / clipCurrent.w * 0.5 + 0.5;

float4 clipPrev = mul(worldToClipPrev, float4(worldPos, 1.0));
float2 uvPrev = clipPrev.xy / clipPrev.w * 0.5 + 0.5;

// Motion vector: where did this pixel come from in the previous frame?
motionVectorOutput[pixel] = uvPrev - uvCurrent;
```

For sky pixels (no hit): project the ray direction through both frames' view-projection matrices to compute sky motion.

---

## 3. Temporal Accumulation: Surface Motion Reprojection

### Current State

```slang
// reblur_temporal_accumulation.cs.slang (current)
uint2 prev_pixel = pixel;  // Identity — no reprojection
```

### New: Reprojection via Motion Vectors

```slang
float2 motionVec = motionVectors[pixel];
float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
float2 prevUV = currentUV + motionVec;
float2 prevPixelF = prevUV * float2(resolution) - 0.5;
```

### Bilinear History Sampling with Occlusion Weights

Sample the 2x2 bilinear neighborhood around `prevPixelF`:

```slang
int2 tl = int2(floor(prevPixelF));
float2 f = prevPixelF - float2(tl);

// For each corner (tl, tr, bl, br):
//   1. Check in-screen bounds
//   2. Load prev_viewZ at that position
//   3. Compute expected prev viewZ from current worldPos + worldToViewPrev
//   4. Compare: abs(prev_viewZ - expected) < threshold * max(|expected|, 1)
//   5. Check normal agreement: dot(prev_normal, current_normal) > thresholdAngle
//   6. Set occlusion weight to 0 if test fails, bilinear weight otherwise

float4 bilinearWeights = float4(
    (1 - f.x) * (1 - f.y),  // tl
    f.x * (1 - f.y),         // tr
    (1 - f.x) * f.y,         // bl
    f.x * f.y                // br
);
float4 occlusionMask;  // 1 if valid, 0 if disoccluded
float4 weights = bilinearWeights * occlusionMask;
float weightSum = dot(weights, float4(1));

// Renormalize or mark as disoccluded
if (weightSum > 1e-6) {
    weights /= weightSum;
    // Sample history with these weights
} else {
    // Fully disoccluded: reset accumulation
}
```

### Catmull-Rom Path

When all 4 bilinear samples pass the occlusion test, use Catmull-Rom 12-tap filter for sharper history:

```slang
bool allValid = all(occlusionMask > 0.5);
if (allValid) {
    // Catmull-Rom (sharpness = 0.5, NRD convention)
    // 12 taps: 4-tap cross in each direction, excluding corners
    // See NRD: _BicubicFilterNoCornersWithFallbackToBilinearFilterWithCustomWeights
    history = CatmullRomSample(prevUV, historyTexture);
} else {
    history = BilinearWeightedSample(weights, historyTexture, tl);
}
```

### Parallax-Based Disocclusion

Adapt disocclusion thresholds based on camera motion magnitude:

```slang
float parallaxInPixels = length(motionVec * float2(resolution));

// Threshold relaxation for small motion
float smallParallax = saturate(1.0 - parallaxInPixels * 4.0);  // [0,1]
float thresholdAngle = 0.906;  // ~25° base (cos)
thresholdAngle -= 0.25 * smallParallax;  // Relax for small motion

// Depth threshold scales with view angle
float depthThreshold = disocclusionThreshold / max(0.05, NoV);
depthThreshold *= frustumSize;  // Projected pixel size at this depth
```

### Footprint Quality

Quality metric for modulating accumulation speed:

```slang
float footprintQuality = dot(bilinearWeights, occlusionMask);  // [0,1]
footprintQuality = sqrt(footprintQuality);

// View angle change quality: penalize grazing angle transitions
float3 Vprev = normalize(cameraDelta + cameraPos - worldPos);
float NoVprev = abs(dot(normal, Vprev));
float sizeQuality = (NoVprev + 1e-3) / (NoV + 1e-3);
sizeQuality = lerp(0.1, 1.0, saturate(sizeQuality * sizeQuality));

footprintQuality *= sizeQuality;
```

Footprint quality modulates the temporal blend weight: lower quality → faster convergence (less history reuse).

---

## 4. Anti-Lag System

### Location

Temporal Stabilization pass (`reblur_temporal_stabilization.cs.slang`).

### Algorithm

Detects when accumulated history lags behind the true signal and reduces accumulation speed:

```slang
float ComputeAntilag(float historyLuma, float currentMeanLuma, float sigma, float accumSpeed) {
    float s = sigma * antilagLuminanceSigmaScale;  // default 2.0
    float magic = antilagLuminanceSensitivity * framerateScale * framerateScale;  // default 3.0

    // Clamp history to acceptable range
    float clamped = clamp(historyLuma, currentMeanLuma - s, currentMeanLuma + s);
    float d = abs(historyLuma - clamped) / (max(historyLuma, clamped) + 1e-6);

    // Response: higher accumSpeed → more sensitive to lag
    d = 1.0 / (1.0 + d * accumSpeed / magic);

    return d;  // [0,1]: 0 = reject history, 1 = keep
}
```

### Integration

```slang
float diffAntilag = ComputeAntilag(diffHistoryLuma, diffMeanLuma, diffSigma, diffAccumSpeed);
float specAntilag = ComputeAntilag(specHistoryLuma, specMeanLuma, specSigma, specAccumSpeed);

// Modulate accumulation speed
diffAccumSpeed = lerp(1.0, diffAccumSpeed, diffAntilag);
specAccumSpeed = lerp(1.0, specAccumSpeed, specAntilag);
```

---

## 5. Framerate Scaling

### C++ Side (ReblurDenoiser)

```cpp
float time_delta_ms = current_frame_time - previous_frame_time;
float framerate_scale = std::max(33.333f / time_delta_ms, 1.0f);  // Base: 30 FPS
```

### Shader Uniform

```slang
float gFramerateScale;  // Passed to all REBLUR passes
```

### Usage Points

| Pass | Usage |
|------|-------|
| Temporal Stabilization | Anti-lag: `magic = sensitivity * framerateScale^2` |
| Temporal Stabilization | Blend weight: `1.0 + 3.0 * framerateScale * w` |

---

## 6. Programmatic Camera Paths

### Purpose

Deterministic, repeatable camera animation for automated test captures. Replaces manual orbit camera interaction during tests.

### CLI Option

```
--camera_animation <path_type>
```

| Path Type | Description |
|-----------|-------------|
| `none` (default) | Static camera, no animation |
| `orbit_sweep` | 360° orbit around scene center over N frames |
| `dolly` | Forward/backward translation along view axis |

### Implementation: CameraAnimator

```
File: libraries/include/scene/component/camera/CameraAnimator.h
      libraries/source/scene/component/camera/CameraAnimator.cpp
```

```cpp
class CameraAnimator {
public:
    enum class PathType { kNone, kOrbitSweep, kDolly };

    void SetPath(PathType type, uint32_t total_frames);
    Transform GetTransform(uint32_t frame_index) const;
    bool IsActive() const;

private:
    PathType path_type_ = PathType::kNone;
    uint32_t total_frames_ = 0;
    Transform initial_transform_;  // Captured at start
};
```

### Integration

In `AppFramework::Tick()`, when `camera_animation != "none"`:
1. Call `CameraAnimator::GetTransform(frame_index)` to get the camera pose
2. Override `OrbitCameraComponent` transform
3. Increment frame counter

### Orbit Sweep Path

```cpp
Transform CameraAnimator::GetOrbitSweepTransform(uint32_t frame) const {
    float t = float(frame) / float(total_frames_);
    float angle = t * 2.0f * M_PI;  // Full 360°
    // Orbit at initial radius around scene center
    float radius = initial_radius_;
    Vec3 pos(cos(angle) * radius, initial_height_, sin(angle) * radius);
    // Look at scene center
    return LookAt(pos, scene_center_, Vec3(0, 1, 0));
}
```

---

## 7. Debug Visualization

### New Debug Modes

Extend the existing `--reblur_debug_pass` or `--debug_mode` system:

| Debug Mode | Output | Purpose |
|------------|--------|---------|
| `motion_vectors` | MV magnitude as heatmap (blue→red) | Verify MV correctness |
| `reprojection_error` | abs(prevUV - currentUV) as heatmap | Visualize disocclusion regions |
| `footprint_quality` | Footprint quality as grayscale | Identify low-confidence regions |
| `antilag` | Anti-lag factor as grayscale | Verify anti-lag activation |
| `accum_speed` | Accumulation speed / maxFrameNum as grayscale | Verify history reset on motion |

These use the existing debug pass infrastructure to write diagnostic data to the output buffer.

---

## 8. Shader Changes Summary

### Modified Shaders

| Shader | Changes |
|--------|---------|
| `ray_trace_split.cs.slang` | Add `worldToClip`/`worldToClipPrev` uniforms; compute motion vectors from hit position |
| `reblur_temporal_accumulation.cs.slang` | Replace identity reprojection with MV-based bilinear/Catmull-Rom sampling; add parallax disocclusion, footprint quality |
| `reblur_temporal_stabilization.cs.slang` | Add anti-lag computation; add framerate scaling to blend weights |

### New Shader Includes

| File | Content |
|------|---------|
| `reblur_reprojection.h.slang` | Bilinear/Catmull-Rom history sampling functions, occlusion test, footprint quality |

### Uniform Additions

```slang
// Added to REBLUR uniform buffer:
float4x4 gWorldToClip;
float4x4 gWorldToClipPrev;
float4x4 gWorldToViewPrev;
float3   gCameraDelta;
float    gFramerateScale;
```

---

## 9. C++ Changes Summary

### Modified Files

| File | Changes |
|------|---------|
| `CameraRenderProxy.h/.cpp` | Add `BeginFrame()`, previous-frame matrix storage |
| `GPURenderer.h/.cpp` | Call `BeginFrame()`, populate ReblurMatrices, pass new uniforms to split PT shader |
| `ReblurDenoiser.h/.cpp` | Add framerate scale computation, pass `gFramerateScale` and `gCameraDelta` to shaders |
| `RenderConfig.h/.cpp` | Add `--camera_animation` option |

### New Files

| File | Purpose |
|------|---------|
| `CameraAnimator.h/.cpp` | Programmatic camera path generation |
| `reblur_reprojection.h.slang` | Shared reprojection utilities |

---

## 10. Test Cases

### Per-Feature Tests

| Test ID | Feature | Setup | Pass Criteria |
|---------|---------|-------|---------------|
| `reblur_mv_smoke` | Motion vectors | `--camera_animation orbit_sweep`, 10 frames | No crash, no NaN in MV buffer |
| `reblur_mv_magnitude` | MV correctness | `--camera_animation orbit_sweep --debug_mode motion_vectors`, 10 frames | MV magnitude > 0 during motion; ~0 when static |
| `reblur_mv_accuracy` | MV precision | `--camera_animation orbit_sweep`, compare reprojected vs actual prev position | Reprojection error < 0.5 pixel for visible surfaces |
| `reblur_reproj_bilinear` | History sampling | `--camera_animation orbit_sweep`, 30 frames | No black pixels in geometry regions; history reuse visible |
| `reblur_reproj_catmullrom` | Catmull-Rom filter | 30 frames static after motion | Sharper than bilinear-only baseline (edge metric) |
| `reblur_disocclusion` | Disocclusion detection | `--camera_animation dolly`, 30 frames | Newly revealed regions fill within 3 frames |
| `reblur_antilag` | Anti-lag activation | `--camera_animation orbit_sweep`, `--debug_mode antilag`, 30 frames | Anti-lag < 1.0 during motion, ~1.0 when static |

### End-to-End Tests

| Test ID | Validates | Setup | Pass Criteria |
|---------|-----------|-------|---------------|
| `reblur_motion_smoke` | No crash under motion | `--camera_animation orbit_sweep --use_reblur true --spp 1`, 60 frames | Screenshot captured, no NaN |
| `reblur_motion_no_ghosting` | No trailing artifacts | `orbit_sweep`, 60 frames, per-pixel temporal diff | No persistent ghost edges; max temporal diff < 0.1 after 5 frames |
| `reblur_motion_reconverge` | Convergence after stopping | `orbit_sweep` 30 frames → static 30 frames | FLIP ≤ 0.12 vs reference after re-convergence |
| `reblur_motion_stability` | Frame-to-frame stability | `orbit_sweep`, 60 frames, per-pixel std across 10 frames | std-dev < 0.03 (relaxed from static 0.015 due to motion) |
| `reblur_motion_regression` | Static camera unchanged | `--camera_animation none --use_reblur true`, 64 frames | FLIP ≤ 0.08 vs existing static baseline |

### Test Execution

```bash
# Build
python build.py --framework glfw

# Motion smoke test
python build.py --framework glfw --run --headless true --pipeline gpu \
    --use_reblur true --spp 1 --camera_animation orbit_sweep --max_spp 60 \
    --test_case screenshot

# Static regression (must not regress)
python build.py --framework glfw --run --headless true --pipeline gpu \
    --use_reblur true --spp 1 --max_spp 64 --test_case screenshot
```

---

## 11. Texture Budget Delta

No new textures required. The existing motion vector buffer (`RG16Float`, 4 MB at 1080p) is already allocated but currently zeroed. All other buffers are reused.

New uniforms add ~320 bytes (two 4x4 matrices + camera delta + framerate scale).

---

## 12. Milestones

| # | Milestone | Description | Test Gate |
|---|-----------|-------------|-----------|
| M1 | Matrix Infrastructure | `CameraRenderProxy::BeginFrame()`, previous-frame matrix storage, `ReblurMatrices` populated with real values, camera delta computed | Unit test: matrices differ after camera move; delta matches displacement |
| M2 | Motion Vectors | `ray_trace_split.cs.slang` outputs correct screen-space MVs using `worldToClip`/`worldToClipPrev` | `reblur_mv_smoke`, `reblur_mv_magnitude`, `reblur_mv_accuracy` |
| M3 | Bilinear Reprojection | Temporal accumulation uses MV-based reprojection with bilinear sampling and basic disocclusion | `reblur_reproj_bilinear`, `reblur_motion_smoke` |
| M4 | Catmull-Rom + Footprint Quality | Adaptive filter (Catmull-Rom when all 4 valid, bilinear fallback), parallax-based disocclusion thresholds, footprint quality weighting | `reblur_reproj_catmullrom`, `reblur_disocclusion` |
| M5 | Anti-Lag + Framerate Scaling | Anti-lag in temporal stabilization, `gFramerateScale` uniform, framerate-normalized anti-lag sensitivity | `reblur_antilag`, `reblur_motion_no_ghosting` |
| M6 | Camera Animator + Test Suite | `CameraAnimator` class, `--camera_animation` CLI, full automated test suite | All motion test cases passing |
| M7 | Non-Regression | Verify static camera quality unchanged; tune motion parameters | `reblur_motion_regression`, `reblur_motion_reconverge`, `reblur_motion_stability` |

---

## 13. Future Work (Not in This Plan)

- **Virtual Motion (VMB)**: Curvature estimation, thin-lens projection for specular reflection tracking
- **Per-object motion vectors**: Animated objects with per-vertex velocity
- **Responsive accumulation**: Roughness-based accumulation cap reduction
- **Material-ID disocclusion**: Cross-material boundary detection
- **History rectification**: Luminance-based history clamping in temporal accumulation (beyond anti-lag)
