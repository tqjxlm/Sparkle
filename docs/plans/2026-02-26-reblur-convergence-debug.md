# ReBLUR Convergence Debugging Notes

**Date:** 2026-02-26
**Issue:** ReBLUR output does not converge to match vanilla GPU pipeline quality after 2048 frames

---

## Reproduction

```bash
# Vanilla (converges)
python3 build.py --framework macos --pipeline gpu --use_reblur false --run --headless true --test_case multi_frame_screenshot --clear_screenshots true

# ReBLUR (does not converge)
python3 build.py --framework macos --pipeline gpu --use_reblur true --run --headless true --test_case multi_frame_screenshot --clear_screenshots true
```

Both capture 5 frames after max_spp (2048) is reached. Screenshots saved to `~/Documents/sparkle/screenshots/`.

---

## Evidence Gathered

### Quantitative frame-to-frame comparison

| Metric | Vanilla | ReBLUR | Ratio |
|--------|---------|--------|-------|
| Mean frame-to-frame diff | 0.000212 | 0.005267 | **25x worse** |
| Max frame-to-frame diff | 0.035 | 0.961 | **27x worse** |
| Pixels changed >1/255 per frame | 0.59% | 6.1% | **10x more** |
| Pixels changed >5/255 per frame | 0.00% | 3.9% | **infinite** |
| Frame 0→4 mean diff | 0.000525 | 0.007364 | 14x worse |
| Mean luminance | 0.4505 | 0.4463 | 0.9% dimmer |

### Quality comparison (vanilla vs reblur frame 4)

- 64.97% of pixels differ by >1/255
- 16.23% of pixels differ by >10/255
- Max quality diff: 0.94 (near full range)

### Instability heatmap analysis

- Instability concentrated on **object surfaces**, especially reflective/specular
- 7.1% of pixels have std_dev > 0.005 across 5 frames
- Worst pixel at (588, 341) swings from 0.95 to 0.0 across frames → **firefly**
- Both top and bottom halves affected (objects appear on both)

### Visual observations

- Table surface appears washed out / over-blurred in ReBLUR
- Reflections on metallic spheres are significantly duller
- Glass sphere renders differently
- Noticeable frame-to-frame flicker on object surfaces

---

## Root Cause Analysis

### Architecture Overview (Pipeline Data Flow)

```
[PathTracer 1spp] → PrePass(blur) → TemporalAccum → HistoryFix → Blur → PostBlur → TemporalStabilization → [output]
                                        ↑                |                                      |
                                        |               temp1 ──── CopyHistoryData ──→ diff_history_
                                        |                                                spec_history_
                                        └──── reads prev history ────────────────────────────────┘
                                                                                      writes antilag speeds
                                                                                      to prev_internal_data_
```

### Root Cause 1: Accumulation Speed Cap at 63 Frames (PRIMARY)

**File:** `shaders/include/reblur_data.h.slang:9`
```c
#define REBLUR_MAX_ACCUM_FRAME    ((1 << 6) - 1)  // = 63
```

**File:** `libraries/include/renderer/denoiser/ReblurDenoiser.h:20`
```cpp
uint32_t max_accumulated_frame_num = 63;
```

The temporal accumulation clamps accumSpeed at 63:
```c
diff_accum_speed = min(prev_diff_accum_speed + 1.0, max_accumulated_frame_num); // ≤ 63
```

At accumSpeed=63, blend weight = 1/(1+63) = 1/64 ≈ **1.56%**. The effective temporal window is only 64 frames. The noise floor is σ/√64 = σ/8.

The vanilla pipeline accumulates up to 2048 frames → noise floor of σ/√2048 = σ/45. ReBLUR has **5.7x more noise**.

The 6-bit packing in RG16Float limits accumSpeed to [0, 63]. Even if `max_accumulated_frame_num` were increased, the packed representation saturates at 63. Increasing the bit width would require changing the texture format or packing scheme.

### Root Cause 2: History Stores Pre-Blur Data

**File:** `ReblurDenoiser.cpp:451`
```cpp
CopyHistoryData(diff_temp1_.get(), spec_temp1_.get()); // HistoryFix output, before Blur/PostBlur
```

NRD's "recurrent" design feeds PostBlur output back as history. Each frame's spatial blur progressively refines the history. Sparkle stores the HistoryFix output (pre spatial blur), so the spatial blur improvement is lost each frame.

This was intentionally changed to prevent "compounding blur" — see MEMORY.md. The compounding was likely caused by insufficient radius decay at high accumSpeed, not by the feedback loop itself.

### Root Cause 3: Antilag Resets from Rotating Poisson Disk

**File:** `ReblurDenoiser.cpp:521`
```cpp
float angle = static_cast<float>(internal_frame_index_) * 2.399963f; // golden angle
```

The Poisson disk spatial kernel rotates every frame (golden angle). Even with identical temporal input, the PostBlur output shifts slightly due to different sampling offsets. This frame-to-frame variation in the 5x5 neighborhood statistics causes the antilag detector to see "divergence":

**File:** `reblur_temporal_stabilization.cs.slang:148-154`
```c
if (diff_accum_speed >= 8.0)
{
    float diff_deviation = abs(stab_diff_luma - diff_mean);
    float diff_threshold = diff_sigma * antilag_sigma_scale;
    diff_antilag = saturate(1.0 - (diff_deviation - diff_threshold) /
                                   max(diff_threshold * antilag_sensitivity, 1e-6));
    diff_accum_speed = lerp(0.0, diff_accum_speed, diff_antilag);
}
```

When antilag triggers: accumSpeed is lerped toward 0. If it resets to 0, next frame's blend weight jumps from 1/64 to 1/1 = **100%**, taking only the new 1-spp noisy sample. This causes:
1. Heavy spatial blur (radius ∝ sqrt(hitFactor * 1.0))
2. Dramatic output change
3. Multi-frame ramp-up to re-accumulate
4. Antilag triggers again → cycle repeats

**The instability on object surfaces is explained by this cycle**: specular/reflective surfaces have high per-frame variance, causing frequent antilag resets.

### Root Cause 4: No Firefly Suppression in Temporal Accumulation

**File:** `reblur_temporal_accumulation.cs.slang:120-123`
```c
// Note: Firefly suppression in temporal accumulation is intentionally omitted.
```

Bright outlier 1-spp samples enter the temporal average unclamped. At accumSpeed=63, a firefly gets 1/64 = 1.6% weight → visible. The worst pixel going from 0.95 to 0.0 across frames is a textbook firefly.

Anti-firefly in HistoryFix only runs for the first 3 frames (history_fix_frame_num=3), then stops. After that, no firefly suppression at all.

---

## Key Code Locations

| Component | File | Line | Purpose |
|-----------|------|------|---------|
| AccumSpeed cap | `reblur_data.h.slang` | 9 | `REBLUR_MAX_ACCUM_FRAME = 63` |
| AccumSpeed packing | `reblur_data.h.slang` | 11-14 | `PackInternalData`: speed/63 in RG16Float |
| Temporal blend | `reblur_temporal_accumulation.cs.slang` | 109-118 | `lerp(history, current, 1/(1+accumSpeed))` |
| History storage | `ReblurDenoiser.cpp` | 451 | `CopyHistoryData(temp1)` — pre-blur |
| Antilag reset | `reblur_temporal_stabilization.cs.slang` | 148-165 | Reduces accumSpeed on divergence |
| Antilag writeback | `ReblurDenoiser.cpp` | 731 | Writes to prev_internal_data_ |
| Poisson rotation | `ReblurDenoiser.cpp` | 521 | `golden angle * frame_index` |
| Blur radius formula | `reblur_blur.cs.slang` | 183 | `maxRadius * sqrt(hitFactor * nonLinear)` |
| Settings defaults | `ReblurDenoiser.h` | 14-34 | Default parameter values |

---

## Attempted / Considered Fixes

### Previously attempted (from MEMORY.md):
- **Removed recurrent blur loop** — history was storing post-blur output, causing compounding spatial blur. Fixed by storing pre-blur (HistoryFix) output. This solved compounding but capped convergence quality.
- **Increased max_history_frames to 4096** — but the 6-bit packing in `REBLUR_MAX_ACCUM_FRAME` limits to 63, so this had no effect beyond 63.

---

## Per-Stage Diagnostic Tests

Ran each debug_pass to isolate which stage introduces instability.

| Stage | debug_pass | Mean frame diff | Pixels >1/255 | Pixels >5/255 |
|-------|-----------|-----------------|---------------|---------------|
| Temporal Accum only | 3 | 0.010 | **43%** | 10% |
| + Spatial blur (PostBlur) | 2 | 0.012 | **58%** | 22% |
| + Stabilization (full) | 99 | 0.005 | **6%** | 4% |
| Vanilla reference | — | 0.0002 | 0.6% | 0% |

### Key Findings:
1. **Temporal accumulation is the core instability source** (43% pixels change per frame)
2. **Spatial blur makes it worse** (43%→58%) because Poisson disk rotates each frame (golden angle)
3. **Stabilization dramatically helps** (58%→6%) but cannot fully compensate for a 63-frame effective window
4. **Fireflies present at all stages** (max_diff ~0.96-1.0 everywhere)

### Confirmed Root Cause: 63-frame accumulation cap

The temporal blend weight at accumSpeed=63 is 1/(1+63) = 1/64 ≈ **1.56%**. The effective running average window is 64 frames. Noise floor = σ/√64 = σ/8.

The vanilla pipeline accumulates over 2048 frames: noise floor = σ/√2048 = σ/45. That's **5.7x less noise**.

The 63-frame cap comes from the 6-bit packing in `reblur_data.h.slang`. However, the RG16Float texture format can represent integers exactly up to 2048 — there's no need for the 6-bit normalization. Removing the normalization and storing raw float values directly would allow accumSpeed up to 2048, matching the vanilla pipeline.

### Effect on spatial blur with higher accumSpeed:
- At accumSpeed=2048: blur radius = maxRadius * sqrt(hitFactor / 2049) ≈ 0.5px → effectively no spatial blur
- At accumSpeed=63: blur radius = maxRadius * sqrt(hitFactor / 64) ≈ 3.75px → noticeable blur that rotates each frame

With high accumSpeed, spatial blur naturally turns off and temporal averaging carries all denoising. This is NRD's intended behavior.

---

## Fix

### Root Cause 5: OutputLimit Clamping on Demodulated Diffuse (DOMINANT)

**File:** `shaders/ray_trace/ray_trace_split.cs.slang:512-513`
```c
float3 diff_clamped = min(output.diffuse_radiance, output_limit);
float3 spec_clamped = min(output.specular_radiance, output_limit);
```

**File:** `libraries/include/renderer/proxy/CameraRenderProxy.h:16`
```cpp
static constexpr Scalar OutputLimit = 6.f;
```

The split path tracer demodulates diffuse by dividing by albedo (line 452):
```c
result.diffuse_radiance /= max(albedo, float3(1e-6, 1e-6, 1e-6));
```

Then the output_limit=6 clamp is applied to the **demodulated** value. For dark surfaces (albedo=0.1), a diffuse radiance of 1.0 becomes demodulated to 10.0, which gets clamped to 6.0. When remodulated (multiplied by albedo=0.1), the result is 0.6 instead of 1.0 — **40% energy loss**.

The vanilla pipeline clamps the **combined** (pre-demodulation) radiance at line 508:
```c
float3 combined_clamped = min(output.total_radiance, output_limit);
```
This clamp is on pre-demodulated values, so it's far less aggressive.

**Verification:** Raising OutputLimit from 6 to 1000 nearly eliminates the energy gap:

| Configuration | Luma @ 2048 spp | vs Vanilla |
|---|---|---|
| **With output_limit=6 (original):** | | |
| Vanilla | 0.450 | baseline |
| Denoiser max_accum=63 | 0.4365 | -3% |
| Denoiser max_accum=2048 | 0.371 | -17% |
| **With output_limit=1000 (clamping disabled):** | | |
| Vanilla | 0.4967 | baseline |
| Denoiser max_accum=63 | 0.4945 | **-0.4%** |
| Denoiser max_accum=2048 | 0.428 | -14% |

With clamping fixed, max_accum=63 (the default) matches vanilla within 0.4%.
The remaining 14% gap at max_accum=2048 is likely float16 precision loss at very high accumSpeed
(α ≈ 1/2049 is below float16's minimum step around typical luminance values).

**Conclusion:** The demodulated clamping is the dominant convergence issue. Fixing it with the default
max_accum=63 setting produces output nearly identical to vanilla.

### Temporal luminance evolution (max_accum=2048, output_limit=6)

| SPP | Denoiser (limit=6) | Denoiser (limit=1000) | Vanilla (limit=6) | Vanilla (limit=1000) |
|-----|--------------------|-----------------------|--------------------|----------------------|
| 10  | 0.062              | 0.409                 | 0.248              | —                    |
| 50  | 0.139              | —                     | 0.242              | —                    |
| 100 | 0.197              | 0.220                 | 0.309              | —                    |
| 500 | 0.342              | —                     | 0.428              | —                    |
| 2048| 0.371              | 0.428                 | 0.450              | 0.4967               |

Early frames are severely affected: at 10 spp, denoiser with limit=6 outputs luma=0.062 vs vanilla=0.248
(75% darker). This is because early frames have the most noise, and demodulated bright outliers
get clamped aggressively.

---

## Disproved Hypotheses

### Hypothesis: Antilag causes energy loss at max_accum=2048
- **Test:** Changed antilag_sigma_scale from 2.0 to 100.0 (effectively disabling antilag)
- **Result:** luma=0.3707 (essentially same as 0.3709 with antilag)
- **Conclusion:** DISPROVED — antilag is NOT the cause of energy loss

### Hypothesis: Float16 texture precision causes energy loss
- **Test:** Changed temp/history textures from RGBAFloat16 to RGBAFloat (float32)
- **Result:** luma=0.3348 (DARKER than float16 at 0.3709)
- **Conclusion:** DISPROVED — float16 precision is NOT the primary cause
- **Note:** Float32 being darker is counterintuitive; may be due to float32 preserving more extreme outlier values that get clamped differently

### Hypothesis: PrePass bilateral filter bias causes energy loss
- **Test:** Set diffuse_prepass_blur_radius=0, specular_prepass_blur_radius=0, min_blur_radius=0
- **Result:** 0.372 vs 0.371 (virtually identical)
- **Conclusion:** DISPROVED — PrePass is NOT the cause

---

## Recommended Fix

Move the output_limit clamping BEFORE demodulation in `ray_trace_split.cs.slang`, or apply
a scaled limit to the demodulated channels:

**Option A (preferred):** Clamp before demodulation
```c
// Clamp total radiance before splitting into diffuse/specular channels
result.diffuse_radiance = min(result.diffuse_radiance, output_limit);
result.specular_radiance = min(result.specular_radiance, output_limit);
// THEN demodulate
result.diffuse_radiance /= max(albedo, float3(1e-6, 1e-6, 1e-6));
```

**Option B:** Scale limit by inverse albedo
```c
float3 inv_albedo = 1.0 / max(albedo, float3(1e-6, 1e-6, 1e-6));
float3 diff_clamped = min(output.diffuse_radiance, output_limit * inv_albedo);
```

Either approach ensures the energy-preserving property of the clamp is maintained
regardless of albedo.
