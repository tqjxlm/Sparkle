# Floor Noise Investigation Progress

## Date: 2026-03-05

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

## Phase 4: Potential Solutions

### For the post-nudge regression (2.15x)

1. **Accept and document**: The 2.15x spike after a small camera nudge
   recovers naturally over ~300-500 frames. For interactive use, this
   means ~5-10 seconds of slightly elevated noise after camera movement.

2. **Faster PT blend after motion**: Temporarily reduce PT_BLEND_RAMP
   when camera motion is detected, so the PT accumulated result takes
   over faster. The PT result doesn't have the frozen noise issue.

3. **Spatial pre-filter before TS**: Apply a small spatial filter to the
   PostBlur output before TS, decorrelating the frozen noise by mixing
   with neighboring pixels. This would reduce the correlation between
   current and history, letting the TS work better.

### For the static convergence (7.55x at 2048 spp)

4. **Increase max_stabilized_frame_num**: At stab=63, the TS reduces 1.94x.
   At stab=256, roughly 4x (more time for decorrelation). But float16
   energy loss is a concern (1-2.6% at 128-256).

5. **Float32 stabilized buffers**: Eliminates float16 energy loss,
   enabling higher stab counts. Doubles stabilized buffer memory.

6. **Reduce PT_BLEND_RAMP from 256 to 128 or 64**: Makes PT blend
   compensate faster, masking the denoiser's noise limitation.

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
