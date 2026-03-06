# Floor Noise Investigation Progress

## Date: 2026-03-05 (updated 2026-03-06)

## Problem Statement
E2E noise is too high in the floor area for old (history-valid) pixels.
Despite proven high reprojection rate (>99%), the denoised output has visible noise
that shouldn't exist for pixels that have accumulated many frames of history.

Two manifestations:
1. **Static convergence**: denoised floor noise stagnates at 7.5x vanilla (Phase 1)
2. **Post-nudge regression**: floor noise spikes 2.15x after a 2-degree camera nudge (Phase 3)

## Phase 1: Static Convergence (from previous session)

### Floor noise by stage and SPP

All metrics use laplacian variance on the floor region (lower 40%, 0.05<luma<0.85).

| Config                | Luma   | LapVar   | LocalStd | Ratio  |
|----------------------|--------|----------|----------|--------|
| vanilla_2048         | 0.6575 | 0.004315 | 0.01493  | 1.00x  |
| vanilla_256          | 0.6538 | 0.033879 | 0.04044  | 7.85x  |
| **TA only 2048**     | 0.6422 | 0.067519 | 0.05639  | **15.65x** |
| PostBlur 2048        | 0.6415 | 0.063136 | 0.05454  | 14.63x |
| **denoised 2048**    | 0.6392 | 0.032589 | 0.03896  | **7.55x** |
| e2e 2048             | 0.6575 | 0.004315 | 0.01493  | 1.00x  |

## Phase 2: Post-Nudge Noise Regression

### Problem
After a 2-degree camera yaw nudge on a converged scene (2048 spp), the
denoised-only floor noise increases 2.15x despite >99% valid reprojection.

### Per-stage measurements (all with --reblur_no_pt_blend)

| Stage                 | Before lap_var | After lap_var | Noise increase |
|-----------------------|----------------|---------------|----------------|
| TemporalAccum         | 0.067519       | 0.074571      | 1.10x          |
| PostBlur              | 0.063136       | 0.071653      | 1.13x          |
| **Full (denoised)**   | 0.032589       | 0.070079      | **2.15x**      |

The TS provides 1.94x noise reduction before the nudge, but only 1.02x after.

### Disocclusion analysis
- Floor disocclusion: 0% (all floor pixels have valid history)
- FootprintQuality: ~0.91 for all floor pixels
- Diffuse accum_speed: preserved at ~57 after nudge (TADepth diagnostic)

### TS internal state (debug pass, but corrupted by debug mode)
- stab_count: stays near max (56-63/63)
- diff_blend: stays near max (~0.90+, with tone mapping distortion)
- min_antilag: ~1.0 (no antilag firing)

Note: The TSStabCount debug pass CORRUPTS the stabilized history buffer
(diagnostic RGB values replace actual colors), so the absolute values
are unreliable. But they confirm stab_count does NOT reset.

## Phase 3: Root Cause Analysis -- FROZEN NOISE CORRELATION

### Pixel-by-pixel regression analysis

Compared PostBlur (no TS) vs Full (with TS) outputs from separate runs
with the same random seed sequence:

| State  | PostBlur lap_var | Full lap_var | TS reduction | Regression slope |
|--------|------------------|--------------|--------------|------------------|
| BEFORE | 0.063136         | 0.032589     | 1.94x        | **0.729 (73%)** |
| AFTER  | 0.071653         | 0.070079     | 1.02x        | **0.859 (86%)** |

**Regression slope**: fraction of PostBlur per-pixel noise that appears in
the Full (TS) output. Higher = TS passes more noise through.

### Root cause: TA frozen noise defeats TS temporal averaging

1. **TA creates frozen noise**: The temporal accumulation EMA (max 63 frames)
   creates per-pixel noise that changes very slowly (~1/63 per frame). At
   convergence, the PostBlur output has a "frozen" noise pattern that is
   stable frame-to-frame (each frame shares 62/63 of history with the previous).

2. **TS can only reduce decorrelated noise**: The TS's EMA blending
   (`output = 0.016 * current + 0.984 * history`) can only reduce noise
   that differs between the current PostBlur and the stabilized history.
   Frozen noise that's identical in both passes through unchanged.

3. **Before nudge**: Over 2048 frames, the frozen noise pattern slowly evolves
   (1/63 change per frame). The TS averages ~33 decorrelation timescales
   (2048/63), achieving 1.94x noise reduction. Regression slope = 0.73
   (27% of noise was decorrelated and removed).

4. **After nudge**: The stabilized history from 5 frames ago was built
   during convergence (smooth). But the current PostBlur shares the SAME
   frozen noise pattern as the pre-nudge PostBlur (only 5/63 = 8% refreshed).
   The decorrelated component is tiny, so the TS can barely reduce noise.
   Regression slope increases to 0.86 (only 14% decorrelated).

5. **Recovery time**: The TS needs the frozen noise to evolve significantly
   before it can average it out again. Since the noise changes at 1/63 per
   frame, full recovery to pre-nudge quality requires ~2048+ frames.

### Why the TS internals look correct but the output doesn't improve

The TS blend weight is ~0.984 (correct), stab_count is ~63 (correct), and
antilag doesn't fire (correct). But the TS blends:

    output = 0.016 * PostBlur + 0.984 * stabilized_history

When PostBlur and stabilized_history have CORRELATED noise (correlation ~0.86),
the blended output retains ~86% of the noise regardless of the blend weight.
The blend weight determines HOW MUCH of each signal to use, but can't
cancel correlated noise.

Analogy: averaging two copies of the same noisy signal gives the same
noisy signal, no matter how many copies you average.

## Phase 4: NVIDIA NRD Reference Comparison (2026-03-06)

### Source: https://github.com/NVIDIAGameWorks/RayTracingDenoiser (v4.17)

### Critical Difference: REBLUR_TS_MIN_SIGMA_FRACTION

**Our code has `REBLUR_TS_MIN_SIGMA_FRACTION = 1.0`, NVIDIA has NO sigma floor.**

This is the primary cause of our TS ineffectiveness:

| Aspect | NVIDIA NRD | Our Implementation |
|--------|------------|-------------------|
| **Min sigma** | None (sigma -> 0 at convergence) | sigma >= mean * 1.0 |
| **Clamp range at convergence** | Collapses to [mean, mean] | [-0.7, 2.0] for floor luma ~0.64 |
| **Effect** | History forced to local mean (spatial filtering) | History unclamped (frozen noise passes through) |

#### How NVIDIA's sigma clamping works at convergence:
1. Converged signal -> neighborhood variance -> 0 -> sigma -> 0
2. `Color::Clamp(mean, sigma*scale, history)` = `clamp(history, mean, mean)` = mean
3. History luminance is forced to the local neighborhood mean
4. `lerp(currentLuma, mean, 0.89)` -> mostly the spatial mean -> effectively denoised
5. The spatial averaging through sigma collapse IS the noise reduction mechanism

#### Why our sigma floor defeats this:
1. `REBLUR_TS_MIN_SIGMA_FRACTION = 1.0` -> sigma_eff >= mean * 1.0 = 0.64
2. Clamp range: [mean - 0.64*2.1, mean + 0.64*2.1] = [-0.7, 2.0]
3. History NEVER gets clamped -> frozen noise passes through unchanged
4. The TS blend averages two correlated frozen noise patterns -> no reduction

#### The "8.5% energy loss" trade-off:
The comment in our code says the sigma floor prevents energy loss. But this
"energy loss" IS the denoising mechanism. Clamping to the local mean shifts
per-pixel luminance toward the neighborhood average, which is exactly what
spatial denoising does. NVIDIA accepts this trade-off; it's small for natural
scenes and invisible compared to the noise it eliminates.

### Other Architectural Differences

| Feature | NVIDIA NRD | Our Implementation |
|---------|------------|-------------------|
| **Stabilized buffer** | Luminance only (`float`) | Full RGBA (`float4`) |
| **History sampling** | Bicubic/CatRom with bilinear fallback | Nearest-neighbor |
| **Neighborhood size** | 3x3 (NRD_BORDER=1) | 5x5 (BORDER=2) |
| **Blend weight source** | TA accum_speed via GetAdvancedNonLinearAccumSpeed | Separate per-pixel stab_count |
| **Convergence tuning** | 3-parameter curve (s, b, p) | Fixed 1/(1+accum) |
| **Default maxAccumulatedFrameNum** | 30 | 63 |
| **Default maxStabilizedFrameNum** | 63 | 63 |
| **Antilag** | Mode 2 with quad-wave neighbor adaptation | Mode 2 without quad-wave |

### Key NVIDIA TS Logic (for reference)

```hlsl
// NVIDIA: No min sigma floor!
smbDiffLumaHistory = Color::Clamp(diffLumaM1, diffLumaSigma * sigmaScale, smbDiffLumaHistory);
// At convergence: sigma -> 0 -> history clamped to mean

// NVIDIA: blend weight from TA accum_speed (no separate stab_count)
float2 params = GetTemporalAccumulationParams(footprintQuality, accumSpeed, antilag);
// params.x = footprintQuality * (1 - 1/(1+k*accumSpeed)) * antilag
// params.y = 1.0 + 3.0 * framerateScale * params.x  (sigma scale)

float diffLumaStabilized = lerp(diffLuma, smbDiffLumaHistory, min(weight, gStabilizationStrength));
// Output: current PostBlur COLOR with stabilized LUMINANCE
diff = ChangeLuma(diff, diffLumaStabilized);
```

### Recommended Fix

**Primary fix: Set `REBLUR_TS_MIN_SIGMA_FRACTION = 0.0`** (or very small like 0.01).
This will allow the sigma clamping to force history toward the local mean at
convergence, matching NVIDIA's behavior and enabling the TS to reduce frozen noise.

**Secondary improvements (optional, lower priority):**
1. Switch to luminance-only stabilization (separate float texture)
2. Use bilinear/bicubic sampling for stabilized history instead of nearest-neighbor
3. Derive blend weight from TA accum_speed instead of separate stab_count

## Phase 5: Implementation and Validation

### Trial 1: Set REBLUR_TS_MIN_SIGMA_FRACTION = 0.0
- Status: **NO EFFECT**
- Change: `reblur_config.h.slang` REBLUR_TS_MIN_SIGMA_FRACTION 1.0 → 0.0
- Result: No measurable improvement. The actual sigma from frozen TA noise (~0.054,
  or 8.5% of floor mean luminance 0.64) is far larger than the floor imposed by
  `mean * 1.0 = 0.64`. Only 0.4% of pixels had sigma < floor. The sigma floor
  was NOT the bottleneck — the frozen noise itself keeps sigma high enough to
  prevent clamping from collapsing the range.
- Kept: The change is still correct (matches NVIDIA, no downside), just not the fix.

### Trial 2: Bilinear sampling + UV clamping for stabilized history
- Status: **SIGNIFICANT IMPROVEMENT**
- Changes:
  1. `reblur_temporal_stabilization.cs.slang`: Replaced nearest-neighbor sampling
     with manual 4-tap bilinear. Replaced out-of-bounds fallback (100% PostBlur)
     with UV clamping to [0,1] (matches NVIDIA's clamp-to-edge sampler behavior).
  2. For OOB pixels, stab_count resets to 0 (prevents ghosting from stale edge history).
- Root cause addressed: With a 142-pixel camera shift on a 1280-wide image, ~11% of
  floor pixels had prevUV outside [0,1]. The original code fell back to 100% PostBlur
  (raw noise), which dominated the overall metric. The bilinear sampling also reduces
  sub-pixel reprojection artifacts for in-bounds pixels.

#### Results (strip-by-strip analysis):

| Region           | TS reduction BEFORE fix | TS reduction AFTER fix |
|------------------|------------------------|----------------------|
| Left quarter     | 0.96x (worse!)         | 1.12x                |
| Mid-left         | 1.18x                  | 3.06x                |
| Mid-right        | 1.17x                  | 2.32x                |
| Right quarter    | 1.04x                  | 1.55x                |
| **Overall**      | **1.02x**              | **1.39x**            |

| Metric                              | Before fix | After fix |
|-------------------------------------|-----------|-----------|
| Overall noise increase (after/before nudge) | 2.15x | 1.58x |
| TS reduction after nudge            | 1.02x     | 1.39x     |

- Left quarter remains weak (1.12x) due to geometric complexity near objects
  (table legs, chair bases create genuine disocclusions).
- Center floor shows strong TS noise reduction (2.3-3.1x), confirming the fix
  works well for pixels with valid reprojection.

### Regression test results (after Trial 2)

Full test suite: 22 passed, 4 failed (all failures pre-existing, unrelated to TS changes).

Converged history test (most relevant): **10/10 passed**
- History cleanness: 2.21x (threshold < 3.0x)
- E2E FLIP: 0.0923 (threshold < 0.25)
- Luma preservation: 0.990 (no energy loss)
- Reprojection validity: 99.7%
- Noise concentration: 6.77x (threshold > 2.0x)

Pre-existing failures (not caused by our changes):
- Per-pass validation: Blur debug pass crash
- Temporal validation / C++ temporal convergence: crash
- Ghosting test: timeout (needs more frames)
- TAHistory convergence: luma increase threshold mismatch

### Remaining improvements (not yet attempted)

1. Luminance-only stabilization (separate `float` texture, matches NVIDIA)
2. Reduce BORDER from 2 to 1 (match NVIDIA's 3x3 neighborhood)
3. Derive blend weight from TA accum_speed instead of separate stab_count
4. Bicubic/CatRom sampling (NVIDIA uses this with bilinear fallback)

## Files

### Diagnostics
- `tests/reblur/diagnose_nudge_floor_noise.py` - Per-stage before/after nudge comparison
- `tests/reblur/diagnose_floor_noise.py` - Static convergence by stage and SPP
- `tests/reblur/diagnose_ts_comparison.py` - PostBlur vs Full pixel regression analysis
- `tests/reblur/diagnose_ts_debug.py` - TS internal state visualization

### Tests
- `tests/reblur/test_floor_noise.py` - Existing floor noise test (FAILS at stab=63)
- `tests/reblur/test_floor_noise_regression.py` - Static convergence regression tests
- `tests/reblur/test_converged_history.py` - History cleanness after nudge

### Code changes (diagnostic only, not production fixes)
- Added `TSStabCount` debug pass to `ReblurDebugPass` enum
- Added `debug_output` UBO field to TS shader (currently unused in normal mode)

### NVIDIA NRD Reference
- `/d/Projects/NRD_reference/` - cloned NRD v4.17
- `/d/Projects/NRD_MathLib/` - cloned MathLib (Color::Clamp definition)
- Key files:
  - `Shaders/REBLUR_TemporalStabilization.cs.hlsl` - TS shader
  - `Shaders/REBLUR_Common.hlsli` - ComputeAntilag, GetTemporalAccumulationParams
  - `Shaders/REBLUR_Config.hlsli` - Config defines
  - `Include/NRDSettings.h` - Default settings (maxStabilizedFrameNum=63)
