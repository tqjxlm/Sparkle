# REBLUR Denoiser Design

## Overview

Implement NVIDIA NRD's REBLUR (Recurrent Blur) algorithm as an optional denoiser in Sparkle's GPURenderer. REBLUR is a self-stabilizing temporal+spatial denoiser that operates on **separate diffuse and specular channels** with normalized hit distances driving adaptive blur radii.

### References

- [NRD GitHub](https://github.com/NVIDIA-RTX/NRD) (v4.17.1)
- Ray Tracing Gems II, Chapter 49: "ReBLUR: A Hierarchical Recurrent Denoiser" by Dmitry Zhdan
- GTC 2020: "Fast Denoising with Self-Stabilizing Recurrent Blurs"

---

## 1. Path Tracer Modifications

### Current State

The GPU path tracer (`shaders/ray_trace/ray_trace.cs.slang`) outputs a single combined HDR color into `imageData` (RGBAFloat) using a moving average for temporal accumulation.

### Required Changes

Split the path tracer output at the **first bounce** into demodulated diffuse/specular channels plus auxiliary G-buffer data.

#### New Output Buffers

| Buffer | Name | Format | Content |
|--------|------|--------|---------|
| Diffuse signal | `diffuseRadianceHitDist` | RGBAFloat16 | Demodulated diffuse radiance (rgb) + normalized hit distance (a) |
| Specular signal | `specularRadianceHitDist` | RGBAFloat16 | Demodulated specular radiance (rgb) + normalized hit distance (a) |
| Normal + roughness | `normalRoughness` | RGBA16Unorm | World normal octahedral (rg) + roughness (b) + material ID (a) |
| View depth | `viewZ` | R32Float | Linear view-space Z depth |
| Motion vectors | `motionVectors` | RG16Float | Screen-space motion vectors (zero for static camera) |
| Albedo | `albedoMetallic` | RGBA8Unorm | Base color (rgb) + metallic (a) for remodulation |

#### Demodulation Strategy

At the primary hit:

1. Evaluate BRDF lobe probabilities to determine diffuse vs specular split
2. **Diffuse radiance** = total indirect diffuse lighting / albedo (remove material color)
3. **Specular radiance** = total indirect specular lighting (no material modulation needed for metals; for dielectrics, divide by Fresnel)
4. **Hit distance** = distance from primary hit to secondary hit (or ray TMax for misses). Normalized via:
   ```
   normHitDist = saturate(hitDist / ((A + viewZ*B) * lerp(1, C, exp2(D * roughness^2))))
   ```
   Default params: A=3.0, B=0.1, C=20.0, D=-25.0

#### SamplePixel Return Struct

```slang
struct PathTracerOutput {
    float3 diffuseRadiance;
    float  diffuseHitDist;
    float3 specularRadiance;
    float  specularHitDist;
    float3 worldNormal;
    float  roughness;
    float  viewZ;
    float3 albedo;
    float  metallic;
    float2 motionVector;
    bool   isHit;  // false for sky pixels
};
```

#### Backward Compatibility

When `use_reblur == false`, the path tracer uses the original combined accumulation path. The auxiliary buffers are not allocated. Zero regression.

---

## 2. REBLUR Pipeline Stages

### Architecture

All REBLUR logic lives in `ReblurDenoiser`, a modular class owned by `GPURenderer`.

### Stage 1: Classify Tiles

- **Shader:** `reblur_classify_tiles.cs.slang`
- **Workgroup:** 16x16
- **Input:** viewZ
- **Output:** tile classification (R8, 1 texel per 16x16 tile)
- **Logic:** If all pixels in a 16x16 tile have `viewZ > denoisingRange`, flag as sky (1.0). Otherwise geometry (0.0).
- **Purpose:** Enables tile-based early-out in all subsequent passes.

### Stage 2: Pre-Pass (Spatial Pre-Filter)

- **Shader:** `reblur_prepass.cs.slang`
- **Input:** tiles, normalRoughness, viewZ, diffuseRadianceHitDist, specularRadianceHitDist
- **Output:** pre-filtered diffuse (temp1), pre-filtered specular (temp1), specHitDistForTracking
- **Logic:**
  - 8-sample Poisson disk bilateral filter
  - Blur radius: `diffusePrepassBlurRadius` (30px) / `specularPrepassBlurRadius` (50px)
  - Bilateral weights: geometry (plane distance) * normal angle * hit distance
  - Fraction scale: 2.0 (more aggressive normal rejection)
- **Purpose:** Reduces initial noise entropy. Computes optimal hit distance for specular tracking.

### Stage 3: Temporal Accumulation

- **Shader:** `reblur_temporal_accumulation.cs.slang`
- **Input:** tiles, normalRoughness, viewZ, motionVectors, prev_viewZ, prev_normalRoughness, prev_internalData, pre-filtered diff/spec, history buffers (diff_history, spec_history, fast histories), specHitDistForTracking
- **Output:** accumulated diff (temp2), accumulated spec (temp2), diff/spec fast history, internal data (data1), surface motion data (data2)
- **Logic:**
  1. **Surface motion reprojection:** Use motion vectors to find previous pixel. Bilinear/Catmull-Rom sampling of previous history.
  2. **Disocclusion detection:** Compare plane distance between current normal/viewZ and reprojected previous. Threshold: `disocclusionThreshold` (0.01). Also check normal agreement and material ID.
  3. **Linear accumulation:** `weight = 1/(1 + accumSpeed)` where `accumSpeed` increments each frame, capped by `maxAccumulatedFrameNum` (30). Reset to 0 on disocclusion.
  4. **Firefly suppression:** Clamp outlier luminance based on relative intensity threshold scaled by accumulation speed.
  5. **Fast history:** Separate short-duration accumulation (`maxFastAccumulatedFrameNum`=6) for clamping reference.
- **Note:** For static camera milestone, motion vectors are zero; reprojection is identity lookup.

### Stage 4: History Fix

- **Shader:** `reblur_history_fix.cs.slang`
- **Input:** tiles, normalRoughness, internalData (data1), viewZ, accumulated diff/spec (temp2), fast histories
- **Output:** fixed diff (temp1), fixed spec (temp1), updated fast histories
- **Logic:**
  - Only active when `frameNum < historyFixFrameNum` (3)
  - 5x5 bilateral kernel with stride = `historyFixBasePixelStride` (14px)
  - Bilateral weights: geometry + normal + hit distance
  - Fills disoccluded regions with plausible data from wide neighborhood
- **Purpose:** Rapid reconstruction of newly revealed areas.

### Stage 5: Blur (Primary Spatial)

- **Shader:** `reblur_blur.cs.slang`
- **Input:** tiles, normalRoughness, viewZ, internalData, fixed diff/spec (temp1)
- **Output:** blurred diff (temp2), blurred spec (temp2), prev_viewZ copy
- **Logic:**
  - 8-sample Poisson disk bilateral filter
  - Radius: `maxBlurRadius * sqrt(hitDistFactor * nonLinearAccumSpeed)`, clamped to `minBlurRadius`
  - Bilateral weights: geometry * normal * hitDistance * material * Gaussian
  - Normal weight threshold: `lobeAngleFraction / (1 + accumSpeed)` (tightens with accumulation)
  - Rotator: per-frame rotation of Poisson disk for temporal variation
- **Purpose:** Primary denoising. Self-stabilizing: radius shrinks as history accumulates.

### Stage 6: Post-Blur (Final Spatial)

- **Shader:** `reblur_post_blur.cs.slang`
- **Input:** tiles, normalRoughness, internalData, viewZ, blurred diff/spec (temp2)
- **Output:** diff_history, spec_history, prev_normalRoughness, prev_internalData, denoised diff/spec (or to temporal stabilization)
- **Logic:**
  - Same as Blur but with `RADIUS_SCALE = 2.0` and `FRACTION_SCALE = 0.5`
  - Writes persistent history for next frame
  - Two permutations: final output vs input to temporal stabilization

### Stage 7: Temporal Stabilization (Optional)

- **Shader:** `reblur_temporal_stabilization.cs.slang`
- **Input:** tiles, normalRoughness, viewZ, internalData, data2, history, stabilized history (ping-pong)
- **Output:** final denoised diff/spec, updated stabilized history
- **Logic:**
  - Load neighborhood luminance into shared memory
  - Compute local mean and variance
  - Clamp slow history to color box: `mean +/- sigma * fastHistoryClampingSigmaScale` (2.0)
  - Anti-lag: detect when accumulated signal lags true signal
  - Controlled by `maxStabilizedFrameNum` (63). Set to 0 to disable.

### Remodulation (Compositing Pass)

- **Shader:** `reblur_composite.cs.slang`
- **Input:** denoised diffuse, denoised specular, albedoMetallic
- **Output:** final HDR color into `scene_texture_`
- **Logic:** `finalColor = denoisedDiffuse * albedo + denoisedSpecular`

---

## 3. Texture Budget

At 1920x1080:

| Category | Count | Format | Size |
|----------|-------|--------|------|
| Input: diffuse signal | 1 | RGBAFloat16 | 16 MB |
| Input: specular signal | 1 | RGBAFloat16 | 16 MB |
| Input: normalRoughness | 1 | RGBA16Unorm | 8 MB |
| Input: viewZ | 1 | R32Float | 8 MB |
| Input: motionVectors | 1 | RG16Float | 4 MB |
| Input: albedoMetallic | 1 | RGBA8Unorm | 8 MB |
| Tiles | 1 | R8 (1/16 res) | <1 MB |
| Temp ping-pong (diff) | 2 | RGBAFloat16 | 32 MB |
| Temp ping-pong (spec) | 2 | RGBAFloat16 | 32 MB |
| History: diff + spec | 2 | RGBAFloat16 | 32 MB |
| History: fast diff + spec | 2 | R16Float | 4 MB |
| History: stabilized (ping-pong) | 4 | RGBAFloat16 | 64 MB |
| Previous: viewZ | 1 | R32Float | 8 MB |
| Previous: normalRoughness | 1 | RGBA16Unorm | 8 MB |
| Previous: internalData | 1 | RG16Uint | 4 MB |
| Internal data (data1, data2) | 2 | RG16Float | 8 MB |
| SpecHitDistForTracking (ping-pong) | 2 | R16Float | 4 MB |
| **Total** | **~26** | | **~246 MB** |

---

## 4. C++ Architecture

### Class: `ReblurDenoiser`

```
File: libraries/include/renderer/denoiser/ReblurDenoiser.h
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp
```

**Responsibilities:**
- Owns all intermediate textures, compute passes, pipeline states, and uniform buffers
- Exposes `Denoise(inputs, settings, matrices)` method
- Manages ping-pong buffer swapping between frames
- Tracks frame index for history ping-pong

**Key types:**

```cpp
struct ReblurSettings {
    float maxBlurRadius = 30.f;
    float minBlurRadius = 1.f;
    float diffusePrepassBlurRadius = 30.f;
    float specularPrepassBlurRadius = 50.f;
    uint32_t maxAccumulatedFrameNum = 30;
    uint32_t maxFastAccumulatedFrameNum = 6;
    uint32_t maxStabilizedFrameNum = 63;
    uint32_t historyFixFrameNum = 3;
    float historyFixBasePixelStride = 14.f;
    float disocclusionThreshold = 0.01f;
    float lobeAngleFraction = 0.15f;
    float roughnessFraction = 0.15f;
    float planeDistanceSensitivity = 0.02f;
    float minHitDistanceWeight = 0.1f;
    float hitDistParams[4] = {3.f, 0.1f, 20.f, -25.f};
    float stabilizationStrength = 1.f;
    float antilagLuminanceSigmaScale = 2.f;
    float antilagLuminanceSensitivity = 3.f;
    float fastHistoryClampingSigmaScale = 2.f;
    bool enableAntiFirefly = true;
};

struct ReblurInputBuffers {
    RHIImage* diffuseRadianceHitDist;
    RHIImage* specularRadianceHitDist;
    RHIImage* normalRoughness;
    RHIImage* viewZ;
    RHIImage* motionVectors;
    RHIImage* albedoMetallic;
};

struct ReblurMatrices {
    Matrix4 viewToClip;
    Matrix4 viewToWorld;
    Matrix4 worldToClipPrev;
    Matrix4 worldToViewPrev;
    Matrix4 worldPrevToWorld;
};
```

### GPURenderer Integration

```cpp
// GPURenderer.h additions:
std::unique_ptr<ReblurDenoiser> reblur_;
RHIResourceRef<RHIImage> diffuse_signal_;
RHIResourceRef<RHIImage> specular_signal_;
RHIResourceRef<RHIImage> normal_roughness_;
RHIResourceRef<RHIImage> view_z_;
RHIResourceRef<RHIImage> motion_vectors_;
RHIResourceRef<RHIImage> albedo_metallic_;

// Separate path tracer shader for REBLUR mode (outputs split channels)
RHIResourceRef<RHIShader> reblur_pt_shader_;
RHIResourceRef<RHIPipelineState> reblur_pt_pipeline_;
RHIResourceRef<RHIComputePass> reblur_pt_pass_;

// Compositing pass
RHIResourceRef<RHIShader> composite_shader_;
RHIResourceRef<RHIPipelineState> composite_pipeline_;
RHIResourceRef<RHIComputePass> composite_pass_;
```

### Render Flow (REBLUR enabled)

```
1. Path tracer (split output) → diffuse/specular + G-buffer
2. ReblurDenoiser::Denoise()
   2a. Classify tiles
   2b. Pre-pass
   2c. Temporal accumulation
   2d. History fix
   2e. Blur
   2f. Post-blur
   2g. Temporal stabilization
3. Composite: diff*albedo + spec → scene_texture_
4. Tone mapping (unchanged)
5. Screen quad (unchanged)
```

### Render Flow (REBLUR disabled)

```
1. Path tracer (original combined output) → scene_texture_
2. Tone mapping (unchanged)
3. Screen quad (unchanged)
```

### Config Additions

```cpp
// RenderConfig.h
bool use_reblur;  // --use_reblur true/false (default: false)
```

`RenderConfig::Validate()` enforces: `use_reblur` only valid when `pipeline == gpu`.

---

## 5. Test Cases

### Per-Stage Validation Tests

| Test ID | Stage | Validates | Method | Pass Criteria |
|---------|-------|-----------|--------|---------------|
| `reblur_tiles` | 1 (Classify) | Sky tiles flagged correctly | Debug output tile map | Sky=1.0, geometry=0.0; matches viewZ > range |
| `reblur_prepass` | 2 (PrePass) | Noise reduction without edge loss | Compare luminance variance: prepass < raw | Variance reduced by >30%; edge FLIP < 0.05 |
| `reblur_temporal_static` | 3 (TemporalAccum) | Accumulation converges under static camera | Run 30 frames, compare against ground truth | FLIP <= 0.12 after 30 frames |
| `reblur_history_length` | 3 (TemporalAccum) | Frame counter increases | Debug-visualize accumSpeed | Monotonically increasing to maxAccumulatedFrameNum |
| `reblur_history_fix` | 4 (HistoryFix) | Disoccluded regions filled | Force history reset (frame 0), check next frame | No black pixels in geometry regions |
| `reblur_blur_converge` | 5 (Blur) | Blur radius decreases over time | Compare spatial variance at frame 5 vs frame 30 | Frame 30 variance < frame 5 variance; edges sharper |
| `reblur_bilateral_edges` | 5 (Blur) | Edges preserved | Check normal discontinuity regions | No visible blur across material boundaries |
| `reblur_post_blur` | 6 (PostBlur) | History written correctly | Verify history buffers are non-zero after first frame | All history textures populated |
| `reblur_stabilization` | 7 (TempStab) | Flicker reduced | Per-pixel std-dev across 10 frames | std-dev < 0.015 |
| `reblur_composite` | Composite | Remodulation correct | diff*albedo + spec vs ground truth | FLIP <= 0.08 |

### End-to-End Tests

| Test ID | Validates | Setup | Pass Criteria |
|---------|-----------|-------|---------------|
| `reblur_e2e_quality` | Denoised output quality | `--use_reblur true --spp 1 --max_spp 64`, static camera, 30+ frames | FLIP <= **0.08** vs 2048spp reference |
| `reblur_e2e_stability` | Temporal stability | 10 consecutive frames after convergence (30+ frames) | Per-pixel luminance std-dev < **0.02** |
| `reblur_e2e_no_regression` | Disabled path unchanged | `--use_reblur false` vs existing baseline | FLIP <= **0.005** |
| `reblur_e2e_smoke` | No crash with REBLUR | `--use_reblur true --test_case screenshot` | Screenshot captured successfully |
| `reblur_e2e_nan_check` | No NaN/Inf in output | Sample random pixels from denoised output | All finite values |

### Test Execution

```bash
# Build with tests
python build.py --framework glfw --test

# Smoke test
python dev/functional_test.py --framework glfw --pipeline gpu --use_reblur true --headless

# Quality test
python dev/functional_test.py --framework glfw --pipeline gpu --use_reblur true \
    --spp 1 --max_spp 64 --headless

# Regression test (disabled)
python dev/functional_test.py --framework glfw --pipeline gpu --use_reblur false --headless
```

---

## 6. File Layout

### New Files

```
shaders/ray_trace/
    ray_trace_split.cs.slang          # Path tracer with split output
    reblur_classify_tiles.cs.slang
    reblur_prepass.cs.slang
    reblur_temporal_accumulation.cs.slang
    reblur_history_fix.cs.slang
    reblur_blur.cs.slang
    reblur_post_blur.cs.slang
    reblur_temporal_stabilization.cs.slang
    reblur_composite.cs.slang

shaders/include/
    reblur_common.h.slang             # Shared REBLUR utilities
    reblur_config.h.slang             # Constants and settings
    reblur_data.h.slang               # Packing/unpacking helpers
    poisson_samples.h.slang           # Pre-computed sample tables

libraries/include/renderer/denoiser/
    ReblurDenoiser.h

libraries/source/renderer/denoiser/
    ReblurDenoiser.cpp

tests/reblur/
    ReblurSmokeTest.cpp               # Smoke + screenshot test
```

### Modified Files

```
libraries/include/renderer/renderer/GPURenderer.h
libraries/source/renderer/renderer/GPURenderer.cpp
libraries/include/renderer/RenderConfig.h
libraries/source/renderer/RenderConfig.cpp
```

---

## 7. Milestones

1. **M1: Infrastructure** - Config, auxiliary buffers, split path tracer, ReblurDenoiser skeleton, composite pass. Verify split output visually.
2. **M2: Spatial-only** - ClassifyTiles + PrePass + Blur + PostBlur. No temporal. Verify edge-preserving blur.
3. **M3: Temporal** - TemporalAccumulation + HistoryFix. Verify convergence under static camera.
4. **M4: Stabilization** - TemporalStabilization. Verify flicker reduction.
5. **M5: Quality tuning** - Parameter tuning to meet FLIP <= 0.08 target. All tests passing.
6. **M6: Camera motion** - Motion vector output, temporal stability under camera animation.
