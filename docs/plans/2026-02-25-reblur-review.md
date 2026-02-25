# REBLUR Implementation Review: Sparkle vs NVIDIA NRD

**Date:** 2026-02-25
**Scope:** Module-by-module comparison of Sparkle's REBLUR implementation against NVIDIA NRD v4.x reference (`external/NRD/Shaders/`)

---

## Executive Summary

The current implementation captures the high-level pipeline structure (ClassifyTiles → PreBlur → TemporalAccum → HistoryFix → Blur → PostBlur → TemporalStabilization → Composite) but diverges significantly from NRD in nearly every pass. The differences fall into three categories:

1. **Fundamental algorithm errors** — wrong formulas that produce incorrect results regardless of tuning
2. **Missing features** — entire subsystems that NRD relies on for correctness
3. **Simplified approximations** — reasonable shortcuts that reduce quality but don't break correctness

The most impactful issues are listed first in each section.

---

## 1. Internal Data Packing (`reblur_data.h.slang`)

### CRITICAL: Single accumulation speed instead of per-channel

| | NRD | Sparkle |
|---|---|---|
| Data1 | `float2(diffAccumSpeed, specAccumSpeed)` | `float2(accum_speed, accum_speed_fast)` |
| InternalData | `uint: diffAccumSpeed + specAccumSpeed + materialID` (bit-packed) | N/A |
| Data2 | `uint: smbOcclusion + vmbOcclusion + virtualHistoryAmount + curvature + smbAllowCatRom` | N/A |

**Sparkle stores a single `accum_speed` shared between diffuse and specular.** NRD tracks them independently because:
- Specular has virtual motion reprojection that may succeed/fail independently of surface motion
- Roughness affects specular accumulation speed via `GetSpecMagicCurve`
- The blur radius for each channel depends on its *own* accumulation speed

**Impact:** Both channels get the same blur radius adaptation and temporal blend weight, which is fundamentally wrong for scenes with mixed rough/smooth materials.

### CRITICAL: MAX_ACCUM_FRAME_NUM discrepancy

| | NRD | Sparkle |
|---|---|---|
| MAX_ACCUM_FRAME_NUM | **63** (6 bits) | **63** (6 bits, but `ReblurSettings::max_accumulated_frame_num` defaults to **30**) |

The C++ default of 30 is passed to the shader as `max_accumulated_frame_num`. NRD uses 63 (the full bit range) as the cap by default. This limits temporal convergence quality.

---

## 2. Temporal Accumulation (`reblur_temporal_accumulation.cs.slang`)

### CRITICAL: No bilinear history sampling

**NRD:** Samples previous frame history using a Catmull-Rom filter with occlusion-aware bilinear fallback. Each of the 4 bilinear taps is individually checked for disocclusion, and weights are set to zero for occluded taps.

**Sparkle:** `uint2 prev_pixel = pixel;` — nearest-neighbor lookup at the exact same pixel coordinate.

Even for a static camera, this is wrong when subpixel jitter (anti-aliasing) is used. But more importantly, NRD's occlusion-aware bilinear sampling is essential for preventing ghosting at depth edges. Nearest-neighbor produces visible blockiness and cannot reject partially-occluded history.

### CRITICAL: No firefly suppression

**NRD:** Clamps outlier luminance during temporal accumulation:
```hlsl
float maxRelativeIntensity = gFireflySuppressorMinRelativeScale +
    REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY / (accumSpeed + 1.0);
float lumaClamped = min(lumaResult, GetLuma(history) * maxRelativeIntensity);
result = ChangeLuma(result, lerp(lumaResult, lumaClamped, antifireflyFactor));
```

**Sparkle:** No firefly suppression at all. Bright outlier samples propagate unclamped into the temporal accumulation.

### CRITICAL: No fast history output

**NRD:** Outputs separate "fast history" (short-duration luma accumulation, capped at `maxFastAccumulatedFrameNum` = 6) that HistoryFix uses for variance clamping. This is a separate output texture (`gOut_DiffFast`, `gOut_SpecFast`).

**Sparkle:** No fast history. The HistoryFix and TemporalStabilization passes have no short-term reference to clamp against.

### MISSING: Virtual Motion Based (VMB) reprojection for specular

NRD computes specular reprojection along the *virtual motion* path (reflecting the hit point through the virtual image plane using curvature estimation). This is critical for correct specular temporal accumulation on curved surfaces and reduces ghosting for mirror-like reflections.

Sparkle has no VMB — specular uses the same surface motion path as diffuse.

### MISSING: Disocclusion test operates on depth difference, not plane distance

**NRD:** Computes disocclusion threshold per-bilinear-tap as:
```hlsl
float disocclusionThreshold = GetDisocclusionThreshold(threshold, frustumSize, NoV);
// frustumSize-scaled, view-angle-dependent
smbPlaneDist = abs(prevViewZ - Xvprev.z);
occluded = smbPlaneDist > disocclusionThreshold;
```
The threshold is `frustumSize * threshold / max(0.05, NoV)` — it scales with distance and viewing angle.

**Sparkle:**
```slang
float z_diff = abs(center_viewZ - prev_z);
float z_threshold = disocclusion_threshold * max(abs(center_viewZ), 1.0);
```
This is a simple relative depth test. It doesn't account for viewing angle (`NoV`), doesn't use `frustumSize` (which depends on projection), and doesn't use the reprojected view-space Z (`Xvprev.z`).

### MISSING: Footprint quality

NRD computes a "footprint quality" metric from the bilinear occlusion weights:
```hlsl
smbFootprintQuality = sqrt(bilinearWeightedOcclusion) * sizeQuality;
```
This modulates the temporal blend weight: poor footprint quality → less history weight. Sparkle has no equivalent.

### MISSING: Separate accumulation speed interpolation

NRD bilinearly interpolates the previous frame's accumulation speed using occlusion-aware weights from the 2x2 footprint. Sparkle reads `prevInternalData[prev_pixel]` at a single integer coordinate.

---

## 3. Spatial Blur / PreBlur / PostBlur (`reblur_blur.cs.slang`)

### CRITICAL: Hit distance factor is not frustum-size-normalized

**NRD:**
```hlsl
float hitDistScale = _REBLUR_GetHitDistanceNormalization(viewZ, gHitDistSettings, roughness);
float hitDist = ExtractHitDist(spec) * hitDistScale;    // denormalize to world units
float hitDistFactor = GetHitDistFactor(hitDist, frustumSize);
// where GetHitDistFactor = saturate(hitDist / frustumSize)
// and frustumSize = gMinRectDimMulUnproject * viewZ (for perspective)
```

**Sparkle:**
```slang
float diff_hit_factor = saturate(center_diff_hit);  // raw normalized [0,1] value
```

The Sparkle implementation uses the *normalized* hit distance directly as the factor. NRD first *denormalizes* it back to world-space units, then divides by `frustumSize` (which is proportional to `viewZ * unproject`). This means:
- **Near objects:** Sparkle under-blurs (hit distance relative to screen coverage is large)
- **Far objects:** Sparkle over-blurs (hit distance relative to screen coverage is small)
- **Roughness dependence is lost:** NRD denormalizes with the roughness-dependent scale, Sparkle doesn't

### CRITICAL: Geometry weight uses depth difference instead of plane distance

**NRD:**
```hlsl
float2 geometryWeightParams = GetGeometryWeightParams(gPlaneDistSensitivity, frustumSize, Xv, Nv);
// a = 1.0 / (sensitivity * frustumSize)
// b = dot(Nv, Xv) * a
// weight = ComputeWeight(dot(Nv, Xvs), a, -b)
//        = SmoothStep(1.0, 0.0, abs(dot(Nv, Xvs) * a + (-b)))
//        = SmoothStep(1.0, 0.0, abs(dot(Nv, Xvs - Xv)) / (sensitivity * frustumSize))
```
This is the **signed plane distance** — the distance from the sample's view-space position to the center pixel's tangent plane. It correctly handles surfaces at grazing angles.

**Sparkle:**
```slang
float w_geom = exp(-abs(center_viewZ - sample_viewZ) / max(abs(center_viewZ) * plane_dist_sensitivity, 1e-6));
```
This is just a **depth difference** test — it only compares Z values, ignoring the surface normal orientation. On surfaces viewed at steep angles, samples at the same depth but different positions on the surface get incorrectly high weight.

### CRITICAL: No GetSpecMagicCurve for specular

**NRD:**
```hlsl
float smc = GetSpecMagicCurve(roughness);
// = (1.0 - exp2(-200.0 * roughness * roughness)) * pow(roughness, 0.25)
```
This curve is used to:
- Scale `minBlurRadius` for specular: `max(blurRadius, gMinBlurRadius * smc)`
- Scale `minHitDistWeight` for specular: `gMinHitDistanceWeight * fractionScale * smc`
- Modulate specular blur radius (via `areaFactor = roughness * hitDistFactor`)
- Modulate stabilization acceleration

**Sparkle:** Uses `lerp(0.3, 1.0, center_roughness)` as a roughness multiplier for specular radius. This is a linear ramp, not the carefully tuned S-curve NRD uses. Mirror-like surfaces (roughness ≈ 0) get a 0.3× factor instead of ≈ 0.0×.

### CRITICAL: No roughness weight for specular

**NRD:** Includes a roughness-difference weight:
```hlsl
float2 roughnessWeightParams = GetRoughnessWeightParams(roughness, roughnessFractionScaled);
w *= ComputeWeight(Ns.w, roughnessWeightParams.x, roughnessWeightParams.y);
```

**Sparkle:** No roughness weight. Blur leaks across roughness boundaries (e.g., a smooth metal next to a rough wall).

### CRITICAL: No material comparison

**NRD:** `w *= CompareMaterials(materialID, materialIDs, gDiffMinMaterial);`

**Sparkle:** No material comparison. Blur leaks across material boundaries.

### MISSING: Anisotropic kernel for specular (world-space sampling)

**NRD specular:** Uses a world-space anisotropic kernel bent toward the reflection direction:
```hlsl
float bentFactor = sqrt(hitDistFactor);
float3 bentDv = normalize(lerp(Nv, Dv, bentFactor));
float2x3 TvBv = GetKernelBasis(bentDv, Nv);
float worldRadius = PixelRadiusToWorld(gUnproject, gOrthoMode, blurRadius, viewZ);
TvBv[0] *= worldRadius * skewFactor;   // narrow along T
TvBv[1] *= worldRadius / skewFactor;   // wide along B
```

**Sparkle:** Isotropic pixel-space Poisson disk sampling for both diffuse and specular. This means specular blur doesn't respect the reflection lobe shape, causing unnecessary blurring perpendicular to the reflection direction.

### MISSING: Lobe trimming in PreBlur

**NRD PreBlur specular:**
```hlsl
float lobeTanHalfAngle = ImportanceSampling::GetSpecularLobeTanHalfAngle(roughness, 0.3);
float lobeRadius = hitDist * NoD * lobeTanHalfAngle;
float minBlurRadius = lobeRadius / PixelRadiusToWorld(...);
blurRadius = min(blurRadius, minBlurRadius);
```
This caps the pre-blur radius to the specular lobe extent, preventing over-blurring.

**Sparkle:** No lobe trimming. Low-roughness specular gets a large pre-blur radius.

### MISSING: Normal weight uses specular lobe half-angle

**NRD:**
```hlsl
float normalWeightParam = GetNormalWeightParam(nonLinearAccumSpeed, gLobeAngleFraction, roughness);
// Uses ImportanceSampling::GetSpecularLobeTanHalfAngle to derive the angle threshold
// from roughness and a configurable percentage of lobe volume
```

**Sparkle:**
```slang
float threshold = fraction * 3.14159265;
return smoothstep(threshold, 0.0, angle);
```
Uses a fixed fraction of π as the threshold, not derived from the actual specular lobe shape.

### MINOR: Non-linear accumulation speed formula

**NRD:** `GetAdvancedNonLinearAccumSpeed(accumSpeed)` — uses a configurable curve with convergence settings:
```hlsl
float f = saturate(accumSpeed / (1.0 + gMaxAccumulatedFrameNum * gConvergenceSettings.z));
float e = gConvergenceSettings.x * lerp(gConvergenceSettings.y, 1.0, f);
return 1.0 / (1.0 + e * accumSpeed);
```

**Sparkle:** `1.0 / (1.0 + accum_speed)` — simple reciprocal. This is NRD's formula with `e = 1.0`, which is reasonable but less controllable.

### MINOR: Weight function shape

NRD uses `ComputeWeight` which is `SmoothStep(1, 0, |x*a+b|)` (non-exponential by default) or `ExpApprox` (rational approximation to exp).

Sparkle uses `exp(-x)` for geometry and `smoothstep` for normal weights. The general behavior is similar but the specific curves differ.

---

## 4. History Fix (`reblur_history_fix.cs.slang`)

### CRITICAL: No anti-firefly clamping

**NRD:** After cross-bilateral filtering, computes neighborhood statistics in a (2*NRD_BORDER+1)² window (excluding center 3x3) and clamps luminance:
```hlsl
float sigma = GetStdDev(M1, M2) * REBLUR_ANTI_FIREFLY_SIGMA_SCALE; // scale = 2.0
luma = clamp(luma, M1 - sigma, M1 + sigma);
```

**Sparkle:** No anti-firefly. Bright outliers pass through unmodified.

### CRITICAL: No fast history clamping

**NRD:** After anti-firefly, clamps against the fast history (short-term luma reference) in a 5x5 window:
```hlsl
float fastSigma = GetStdDev(fastM1, fastM2) * gFastHistoryClampingSigmaScale;
float lumaClamped = clamp(luma, fastM1 - fastSigma, fastM1 + fastSigma);
luma = lerp(lumaClamped, luma, 1.0 / (1.0 + frameNum * 2.0));
```

**Sparkle:** No fast history clamping.

### MISSING: Weight based on accumulation speed

NRD's HistoryFix weights samples by their accumulation speed: `w *= 1.0 + UnpackData1(gIn_Data1[pos]).x` — more accumulated neighbors are trusted more.

Sparkle doesn't weight by neighbor accumulation speed.

### MISSING: Hit distance weight in HistoryFix

**NRD:** Uses A-trous hit distance weight:
```hlsl
float d = hs - hitDist;
w *= exp(-d * d * hitDistWeightNorm);
// where hitDistWeightNorm = 1.0 / (0.5 * nonLinearAccumSpeed)
```

**Sparkle:** No hit distance weight in HistoryFix.

### MISSING: Roughness weight for specular in HistoryFix

NRD uses `ComputeExponentialWeight(Ns.w * Ns.w, relaxedRoughnessWeightParams.x, .y)`.

Sparkle: no roughness weight.

### MISSING: Corner-skipping pattern

NRD skips corner samples in the 5x5 kernel: `if(abs(i) + abs(j) == REBLUR_HISTORY_FIX_FILTER_RADIUS * 2) continue;`

Sparkle samples all 25 positions (including corners).

### MINOR: Stride calculation

**NRD:** `stride = gHistoryFixBasePixelStride / 2.0 * (2.0 / FILTER_RADIUS) * (frameNum < historyFixFrameNum)` with quad-read adaptation.

**Sparkle:** `stride = history_fix_stride * (1.0 - accum_speed / history_fix_frame_num)`. The decay is linear in accum_speed rather than a hard cutoff per-channel.

---

## 5. Temporal Stabilization (`reblur_temporal_stabilization.cs.slang`)

### CRITICAL: Neighborhood window is too small (3x3 vs 5x5+)

**NRD:** Uses `NRD_BORDER = 2` → 5x5 neighborhood (or larger with `NRD_BORDER = 4` for anti-firefly). Invalid (sky) pixels are replaced with center value (`d == REBLUR_INVALID ? center : d`).

**Sparkle:** `BORDER = 1` → 3x3 neighborhood. Small neighborhood = poor variance estimates = either too much clamping (losing signal) or too little (not suppressing ghosts).

### CRITICAL: No antilag detection

**NRD:**
```hlsl
float diffAntilag = ComputeAntilag(smbDiffLumaHistory, diffLumaM1, diffLumaSigma, smbFootprintQuality * data1.x);
data1.x = lerp(diffMinAccumSpeed, data1.x, diffAntilag);
```
When the history diverges from the current frame's neighborhood statistics, antilag reduces the accumulation speed, causing the denoiser to "forget" stale history faster. This is critical for adapting to scene changes.

**Sparkle:** No antilag. Stale history persists until natural decay.

### CRITICAL: No accumulation speed writeback

**NRD's TemporalStabilization writes:**
```hlsl
gOut_InternalData[pixelPos] = PackInternalData(data1.x, data1.y, materialID);
```
This is where the *final* accumulation speed (modified by antilag) gets written for the next frame.

**Sparkle's TemporalStabilization does NOT write back accumulation speed.** The next frame sees the un-modified speed from TemporalAccumulation, meaning antilag could never take effect even if it existed.

### CRITICAL: Luminance-only clamping instead of color-space clamping

**NRD:** Operates in YCoCg space (if `REBLUR_USE_YCOCG = 1`, which is default). `GetLuma()` returns the Y component. `ChangeLuma()` scales all channels proportionally. `Color::Clamp()` clamps luminance within the box.

**Sparkle:** Works in linear RGB. The stabilization shader computes luminance via `dot(color, float3(0.2126, 0.7152, 0.0722))` and clamps only the scalar luminance, then rescales the full RGB color. This can cause color shifts when clamping, because the RGB ratios of the history may differ from the current frame's.

### MISSING: Surface + virtual motion reprojection for stabilized history

**NRD:** Stabilization samples previous stabilized history using the same SMB+VMB dual-path reprojection as temporal accumulation, with Catmull-Rom filtering and occlusion-aware weights.

**Sparkle:** `prevStabilizedDiff[pixel]` — nearest-neighbor at the same pixel coordinate.

### MISSING: Adaptive sigma scale

**NRD:** `sigma * diffTemporalAccumulationParams.y` where `.y = 1.0 + 3.0 * gFramerateScale * w` (tighter clamping as more history accumulates).

**Sparkle:** `sigma * fast_history_sigma_scale` (constant). No frame-rate adaptation.

### MISSING: Specular acceleration factor

NRD modulates specular stabilization weight by `GetSpecMagicCurve(roughness)` and `RemapRoughnessToResponsiveFactor(roughness)` to allow low-roughness specular to converge faster.

Sparkle treats specular the same as diffuse.

---

## 6. Path Tracer Split (`ray_trace_split.cs.slang`)

### CRITICAL: Not using YCoCg color space

**NRD expects:** Radiance packed via `REBLUR_FrontEnd_PackRadianceAndNormHitDist()` which converts `Linear RGB → YCoCg`:
```hlsl
radiance = _NRD_LinearToYCoCg(radiance);
return float4(radiance, normHitDist);
```
All internal luminance computations (`GetLuma()`) then just read `.x` (the Y component).

**Sparkle:** Writes linear RGB directly. All luminance computations in subsequent passes use `dot(color, float3(0.2126, ...))`. While this is a valid luminance calculation, NRD's entire weight computation pipeline assumes YCoCg, and the constants were tuned for it.

### ISSUE: Diffuse/specular split heuristic

**NRD expects:** The integrator to properly split based on BRDF lobe evaluation:
- Diffuse = indirect diffuse lighting / albedo
- Specular = indirect specular lighting (demodulated by Fresnel for dielectrics)

**Sparkle:** Uses a post-hoc heuristic based on reflection direction cosine angle:
```slang
if (mat_param.metallic > 0.5 || (cos_angle > 0.5 && mat_param.roughness < 0.5))
    is_diffuse_path = false;
else
    is_diffuse_path = true;
```
Hard thresholds at 0.5 for metallic, cos_angle, and roughness. This misclassifies rays at the boundaries.

### ISSUE: No specular demodulation for dielectrics

**NRD's documentation states:** Specular radiance should be demodulated by dividing by the Fresnel term for dielectrics (to allow the denoiser to work in a more uniform signal space).

**Sparkle:** No specular demodulation — specular is written as-is.

### MINOR: Motion vectors always zero

The shader hardcodes `motionVectorOutput[pixel] = float2(0, 0)`. For static camera this is correct, but subpixel jitter from AA would benefit from proper motion vectors.

---

## 7. Pipeline Orchestration (`ReblurDenoiser.cpp`)

### CRITICAL: History feedback loop removed (breaks "Recurrent" in REBLUR)

The MEMORY.md notes that the "recurrent blur loop" was removed as a convergence fix — history now stores the temporal-accumulation output (pre-spatial-blur) instead of the post-blur output.

**NRD's design:** PostBlur writes its output to `gOut_Diff`/`gOut_Spec`, which becomes the history for the next frame's TemporalAccumulation. This creates the "recurrent" feedback: `temporal_blend(new_preblurred, old_postblurred)`. The spatial blur contribution gradually diminishes as `accumSpeed` grows (blend weight → 0), so it converges rather than compounds.

**Sparkle:** `CopyHistoryData(diff_temp1_, spec_temp1_)` saves the HistoryFix output (which is pre-spatial-blur) as history. This removes the core recurrent property of REBLUR.

The "compounding blur" issue that motivated this change was likely caused by other bugs (wrong accumulation speed management, wrong blur radius scaling, or the single-channel accumulation speed) rather than the feedback loop itself. With correct radius adaptation and accumulation speed, the feedback converges to a stable result.

### ISSUE: GPU→GPU copies instead of shader-side writes

Sparkle uses 3 GPU copy operations per frame:
1. `CopyHistoryData` — copies temp1 → diff_history/spec_history + internal_data → prev_internal_data
2. `CopyPreviousFrameData` — copies viewZ → prev_viewZ, normalRoughness → prev_normalRoughness
3. `CopyStabilizedHistory` — copies stabilized → denoised_diffuse/denoised_specular

NRD writes these directly from the shader (PostBlur writes history, copies G-buffer). The copies add unnecessary GPU memory bandwidth and synchronization overhead.

### ISSUE: Single shared Blur shader for PreBlur/Blur/PostBlur

Using `blur_pass_index` to branch at runtime instead of compile-time specialization. NRD uses separate shader permutations with `#define REBLUR_SPATIAL_MODE` resolved at compile time, enabling the compiler to optimize dead code paths.

---

## 8. Classify Tiles (`reblur_classify_tiles.cs.slang`)

This pass is correct and matches NRD's logic. Minor difference: NRD uses tiles for early-out in subsequent passes, while Sparkle's passes check `viewZ > denoising_range` per-pixel instead of checking the tile map.

---

## 9. Composite (`reblur_composite.cs.slang`)

The composite logic `finalColor = diff.rgb * albedo.rgb + spec.rgb` is correct and matches NRD's expected remodulation.

If YCoCg packing were used, the denoised output would need `REBLUR_BackEnd_UnpackRadianceAndNormHitDist()` (YCoCg → Linear) before compositing.

---

## Summary: Priority of Issues

### Must Fix (fundamental correctness)

| # | Issue | Passes Affected |
|---|-------|-----------------|
| 1 | Separate diffuse/specular accumulation speeds | All |
| 2 | Frustum-size-normalized hit distance factor | Blur, PreBlur, PostBlur |
| 3 | Plane-distance geometry weight (not depth difference) | Blur, PreBlur, PostBlur, HistoryFix |
| 4 | Restore recurrent blur feedback (history = post-blurred) | Orchestration |
| 5 | Bilinear history sampling with occlusion-aware weights | TemporalAccum |
| 6 | Anti-firefly clamping | HistoryFix |
| 7 | Fast history output + clamping | TemporalAccum, HistoryFix |
| 8 | Antilag detection + accumulation speed writeback | TemporalStab |
| 9 | Larger neighborhood window (5x5 minimum) | TemporalStab |
| 10 | GetSpecMagicCurve for specular radius/weight modulation | Blur, PreBlur, PostBlur, HistoryFix, TemporalStab |

### Should Fix (significant quality impact)

| # | Issue | Passes Affected |
|---|-------|-----------------|
| 11 | YCoCg color space for internal operations | All shaders |
| 12 | Roughness weight for specular | Blur, PostBlur, HistoryFix |
| 13 | Material comparison in bilateral weights | All spatial passes |
| 14 | Anisotropic kernel for specular (world-space sampling) | Blur, PostBlur |
| 15 | Lobe trimming in PreBlur | PreBlur |
| 16 | Proper specular lobe-based normal weight | All spatial passes |
| 17 | Firefly suppression in TemporalAccum | TemporalAccum |
| 18 | Proper disocclusion threshold (frustumSize, NoV) | TemporalAccum |
| 19 | Footprint quality metric | TemporalAccum, TemporalStab |
| 20 | Adaptive sigma scale in stabilization | TemporalStab |

### Nice to Have (polish / advanced features)

| # | Issue | Passes Affected |
|---|-------|-----------------|
| 21 | Virtual Motion Based reprojection for specular | TemporalAccum, TemporalStab |
| 22 | Catmull-Rom history sampling | TemporalAccum, TemporalStab |
| 23 | Quad-read neighbor adaptation | Blur, HistoryFix |
| 24 | Specular demodulation by Fresnel | Path tracer |
| 25 | Proper diffuse/specular split (BRDF lobe evaluation) | Path tracer |
| 26 | Shader-side history writes (eliminate GPU copies) | Orchestration |
| 27 | Compile-time pass specialization | Blur shader |
