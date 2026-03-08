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

## Phase 6: Reproduction Against `test_converged_history.py` (2026-03-06 afternoon)

### Task 1: Re-run the user-referenced end-to-end harness
- Status: **DONE**
- Command: `python3 tests/reblur/test_converged_history.py --framework macos --skip_build`
- Run mode: invoked through `build.py` for every capture, per user instruction.
- Result: **9 passed, 1 failed**

### Findings
- Run 0 (vanilla baseline) passed and produced clean before/after references.
- Run 1 (full end-to-end with PT blend) still looks materially worse than Run 0:
  - **E2E FLIP vs vanilla: 0.1736**
  - **E2E luma ratio: 0.9214**
- Run 2 + Run 3 show the residual noise is not explained by disocclusion:
  - **Valid reprojection: 99.7%**
  - **Footprint quality: 0.910**
  - **Disoccluded pixels: 0.3% of geometry**
- History-valid pixels remain too noisy:
  - **Reblur history HF residual: 0.060334**
  - **Vanilla history HF residual: 0.018369**
  - **History noise ratio: 3.28x**
- Current thresholds still hide the real regression in Run 1:
  - `E2E_FLIP_MAX = 0.25` passes even though the floor visibly regresses.
  - `HISTORY_NOISE_RATIO_MAX = 3.0` was already loose and now fails anyway.

### Conclusion after Task 1
- The last TS sampling fix improved the metric relative to older trials, but it did
  **not** solve the camera-nudge floor-noise issue.
- The remaining problem is real denoiser residual noise on history-valid pixels,
  not a reprojection coverage failure.
- Next task: run per-stage diagnostics again and identify which pass keeps the
  floor noisy after the nudge before tightening the thresholds.

### Task 2: Re-run per-stage floor diagnostics on current-source Release build
- Status: **DONE**
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 tests/reblur/diagnose_nudge_floor_noise.py --framework macos --skip_build --config Release`
- Reason: verify the issue against a fresh build from `HEAD` and isolate the
  exact pass responsible for the visible floor noise.

### Findings from Task 2
- A fresh Release build reproduces the same semantic failure as Task 1:
  - **Run 1 FLIP: 0.1915**
  - **History noise ratio: 3.28x**
- Per-stage floor metrics show the regression is still isolated to **Temporal Stabilization**:

| Stage | Before lap_var | After lap_var | After/Before |
|-------|----------------|---------------|--------------|
| TemporalAccum | 0.536570 | 0.450352 | 0.84x |
| PostBlur | 0.503590 | 0.483541 | 0.96x |
| **Full (denoised-only)** | **0.125103** | **0.157013** | **1.26x** |

- Floor reprojection remains effectively perfect:
  - **Floor + history:** 368538 pixels
  - **Floor + disoccluded:** 102 pixels
- Interpretation:
  - The nudge itself is **not** increasing TA or PostBlur floor noise.
  - TS still reduces noise strongly in both states, but its output remains too
    noisy in absolute terms and regresses modestly after the nudge.
  - This means the unresolved issue is no longer history sampling coverage;
    it is the TS reconstruction model itself.

### Next step after Task 2
- Trial a TS model closer to NRD's luminance stabilization path so the final
  output uses current-frame spatial color while only stabilizing luminance.

### Task 3: Restore long TS history without reintroducing energy loss
- Status: **DONE**
- Changes:
  - Kept `max_accumulated_frame_num = 511` (the earlier cap-only trial was the
    only thing that meaningfully reduced TA/PostBlur floor noise).
  - Changed stabilized ping-pong history from **RGBA16F** to **RGBA32F**.
  - Added `reblur_copy_stabilized.cs.slang` so TS can write float32 history and
    only quantize once when copying back into the float16 composite inputs.
  - Raised `max_stabilized_frame_num` back to **255** now that the recurrent
    float16 quantization loop is gone.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`
  - `python3 tests/reblur/diagnose_nudge_floor_noise.py --framework macos --skip_build --config Release`

### Findings from Task 3
- The precision fix materially improved the exact user-reported failure mode:
  - **Run 1 E2E FLIP vs vanilla:** `0.1954`
  - **Run 1 E2E luma ratio:** `0.9185`
  - **History noise ratio:** `1.33x` (down from `3.28x`)
- The history-valid floor is now close to the vanilla and pre-nudge floor:
  - **Run 1 history-valid floor local_std / vanilla-after:** `1.069x`
  - **Run 1 history-valid floor local_std / Run 1 before:** `1.072x`
- Denoised-only floor quality is no longer the dominant regression:
  - **Denoised floor local_std before:** `0.01756`
  - **Denoised floor local_std after:** `0.02010`
  - **After/before:** `1.20x`
- Per-stage floor laplacian variance still rises after the nudge, but the
  remaining increase is modest in absolute noise terms:

| Stage | Before lap_var | After lap_var | After/Before |
|-------|----------------|---------------|--------------|
| TemporalAccum | 0.049565 | 0.061121 | 1.23x |
| PostBlur | 0.035486 | 0.047250 | 1.33x |
| Full (denoised-only) | 0.007089 | 0.018954 | 2.67x |

### Conclusion after Task 3
- The cap-only fix was insufficient because `max_stabilized_frame_num > 63`
  used to reintroduce TS energy loss through recurrent float16 quantization.
- Keeping stabilized history in float32 removes that failure mode and allows a
  longer TS horizon (`255`) without bringing back the old luminance regression.
- This is the first configuration that both:
  - preserves energy well enough for the full pipeline, and
  - brings Run 1 history-valid floor noise close to the vanilla / pre-nudge floor.

### Task 4: Tighten `test_converged_history.py` against the real floor regression
- Status: **DONE**
- Changes:
  - Tightened `HISTORY_NOISE_RATIO_MAX` from `3.0` to `1.6`.
  - Tightened `E2E_FLIP_MAX` from `0.25` to `0.22`.
  - Added two explicit Run 1 history-valid floor checks:
    - `after / vanilla-after <= 1.15`
    - `after / before <= 1.15`
- Command:
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`

### Findings from Task 4
- The updated regression now validates the failure mode the user called out
  instead of relying on the old loose aggregate proxy:
  - **Run 1 floor after / vanilla-after:** `1.069x`
  - **Run 1 floor after / before:** `1.072x`
  - **History cleanness:** `1.33x`
  - **E2E FLIP:** `0.1954`
- Final verification result: **12 passed, 0 failed**

### Final state after Phase 6
- Production fix:
  - stabilized history uses float32 precision
  - `max_accumulated_frame_num = 511`
  - `max_stabilized_frame_num = 255`
- Regression coverage now directly checks:
  - history-valid floor noise in Run 1 against Run 0
  - history-valid floor noise in Run 1 against Run 1 before
  - overall history cleanness and whole-frame FLIP as secondary guards

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

## Phase 7: Residual E2E FLIP / Dim Floor Investigation (2026-03-06)

### Task 5: Re-isolate the remaining dim-floor bias by pass
- Status: **DONE**
- Commands:
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --reblur_debug_pass Passthrough --clear_screenshots true --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --reblur_debug_pass PrePass --reblur_no_pt_blend true --clear_screenshots true --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --reblur_debug_pass TemporalAccum --reblur_no_pt_blend true --clear_screenshots true --config Release`

### Findings from Task 5
- The raw split path tracer is still correct:
  - `Passthrough before floor luma = 0.649144`
  - This matches the vanilla floor baseline and rules out the split PT / combined accumulation path as the source of the dim-floor regression.
- The denoiser path is where the bias appears:
  - `TemporalAccum before floor luma = 0.608567`
  - This is already ~`6.3%` below the passthrough floor on the same mask, so the persistent dimness exists before HistoryFix, PostBlur, or Temporal Stabilization.
- The `PrePass` debug view was much darker (`0.341283` on the same mask), but that result is diagnostic-only:
  - `PrePass` debug exits before updating temporal history, so it repeatedly runs as an isolated single-frame spatial filter.
  - That makes it useful for locating aggressive spatial bias, but not a direct proxy for the converged full pipeline.
- Conclusion:
  - The remaining E2E "After is dimmer than Before" problem is not a TS regression anymore.
  - It is a denoiser under-exposure problem that is already present by `TemporalAccum`, likely driven by overly permissive spatial reuse / hit-distance weighting in the blur inputs that feed TA.

### Task 6: Test stable G-buffer / lobe split hypotheses against the floor-luma gap
- Status: **DONE**
- Trials:
  1. Replaced the denoiser G-buffer path with a deterministic pixel-center primary hit for
     `normalRoughness`, `viewZ`, `motionVectors`, and `albedoMetallic`, while leaving the
     radiance ray stochastic.
  2. Replaced the heuristic primary-lobe classification with the actual sampled lobe.
  3. Split primary-hit NEE into diffuse and specular components instead of forcing all NEE
     into the diffuse channel.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`
  - `python3 tests/reblur/diagnose_nudge_floor_noise.py --framework macos --skip_build --config Release`

### Findings from Task 6
- The deterministic G-buffer trial materially improved denoised before/after stability, but
  it did **not** remove the persistent floor bias versus vanilla:
  - `denoised_after / denoised_before = 0.998`
  - `denoised_after / vanilla_after = 0.952`
  - `e2e_after / e2e_before = 0.950`
- The end-to-end metrics only moved marginally:
  - `E2E FLIP = 0.1900`
  - `E2E luma ratio = 0.9228`
- Stage-localized floor luma still showed the bias entering no later than TA:
  - `TemporalAccum before = 0.6136`
  - `PostBlur before = 0.6123`
  - `Full denoised before = 0.6188`
- The sampled-lobe / split-NEE cleanup compiled and is architecturally more correct, but it
  produced no meaningful change in the floor-luma ratios.
- Conclusion:
  - The remaining gap is not from the old stochastic G-buffer or the old lobe-classification
    heuristic.
  - It is a persistent denoised-vs-vanilla brightness gap.

### Task 7: Prototype stabilized remodulation albedo
- Status: **DONE**
- Trial:
  - Switched `albedoMetallic` to use the stochastic primary-hit sample average from the split
    tracer and added a new `reblur_stabilize_albedo.cs.slang` pass that temporally stabilizes
    the remodulation albedo before the final full composite.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`

### Findings from Task 7
- The stabilized-albedo prototype slightly improved whole-frame metrics, but it did **not**
  move the floor-luma regression the user called out:
  - `E2E FLIP: 0.1900 -> 0.1891`
  - `E2E luma ratio: 0.9228 -> 0.9256`
  - `e2e_after / vanilla_after floor ratio: 0.95068 -> 0.95058` (no real change)
  - `denoised_after / vanilla_after floor ratio: 0.95198 -> 0.95229` (no real change)
- History-valid floor noise stayed good, but the denoised history cleanness actually moved the
  wrong way (`1.12x` vs vanilla, still passing but worse than the prior trial).
- Conclusion:
  - The missing fix is **not** stabilized remodulation albedo.
  - The visible `After` dimness is now strongly isolated as a deeper denoised-vs-vanilla
    brightness gap; the next plausible architectural fix is a post-composite temporal history
    (or equivalent reprojection of the displayed color), not another tweak to the current
    remodulation inputs.

### Task 8: Add renderer-owned final displayed-color history
- Status: **DONE**
- Trial:
  - Added `reblur_final_history.cs.slang` plus `GPURenderer`-owned history textures for the
    displayed linear color and previous `viewZ` / `normalRoughness`.
  - Ran the new pass after `reblur_composite`, reprojecting the previous displayed color with
    motion vectors and the same depth/normal/material validity tests as REBLUR.
  - Reused current REBLUR accumulation speed as the displayed-history blend weight so only
    already-converged pixels keep strong history after a camera nudge.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`

### Findings from Task 8
- This is the first trial that fixed the user-visible Run 1 regression instead of only moving
  denoised-only metrics:
  - `E2E FLIP = 0.1215`
  - `Run 1 floor local_std after / vanilla = 0.885x`
  - `Run 1 floor local_std after / before = 1.037x`
  - `Run 1 floor luma after / vanilla = 0.992x`
  - `Run 1 floor luma after / before = 0.994x`
  - `History cleanness = 1.12x`
  - `test_converged_history.py = 14 passed, 0 failed`
- The denoised-only floor bias is still present:
  - `denoised_after / vanilla_after floor ratio = 0.952x`
  - `denoised_after / denoised_before floor ratio = 0.998x`
- Conclusion:
  - The remaining visible end-to-end dimming/noise issue was a **displayed-color history**
    problem, not another REBLUR internal radiance-history problem.
  - Run 1 now preserves converged brightness and low noise across valid-history pixels after
    the camera nudge, which makes it reasonable to tighten the Run 1 regression thresholds.

### Task 9: Tighten Run 1 regression thresholds to the actual fixed behavior
- Status: **DONE**
- Trial:
  - Tightened `test_converged_history.py` around the corrected full-pipeline behavior.
  - Added explicit Run 1 history-valid floor **luma** checks so floor dimming can no longer
    pass as long as noise happens to be low.

### Findings from Task 9
- New Run 1 thresholds:
  - `E2E_FLIP_MAX = 0.14`
  - `HISTORY_NOISE_RATIO_MAX = 1.3`
  - `E2E_FLOOR_HISTORY_VS_VANILLA_MAX = 1.05`
  - `E2E_FLOOR_HISTORY_AFTER_BEFORE_MAX = 1.05`
  - `E2E_FLOOR_LUMA_RATIO in [0.98, 1.02]`
- Verified with `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`
  after the threshold change:
  - `E2E FLIP = 0.1215`
  - `Run 1 floor luma after / vanilla = 0.992x`
  - `Run 1 floor luma after / before = 0.994x`
  - `14 passed, 0 failed`

### Additional note
- `python3 tests/reblur/diagnose_energy_loss.py --framework macos --skip_build --spp 64 --config Release`
  was attempted, but it failed inside the helper because its subprocess calls to `build.py`
  hit `git submodule update --init --recursive` exit `128`. Direct `build.py` invocations from
  the repo root continued to work.

### Task 10: Restore valid sphere reprojection after the failed previous-depth experiment
- Status: **DONE**
- Trials:
  - Added a sphere-local analysis using the lower-middle blue sphere mask from the vanilla
    `after` screenshot because the whole-frame Run 1 thresholds were not specific enough to
    catch this regression.
  - Ran native macOS `TADisocclusion` and confirmed the current shader was incorrectly marking
    the entire blue sphere as disoccluded after the 2 degree camera nudge.
  - Reverted the experimental previous-surface-view-Z reprojection path, restoring TA's
    center-sampled `prevViewZ` depth validation while keeping the newer shared bilinear-weight
    reuse / Catmull-Rom safety changes.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --reblur_debug_pass TADisocclusion --clear_screenshots true --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --reblur_no_pt_blend true --clear_screenshots true --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --clear_screenshots true --config Release`

### Findings from Task 10
- The failed previous-depth trial was the direct cause of the sphere-wide history loss:
  - Before revert: `TADisocclusion` on the blue sphere was `0% history / 100% disoccluded`.
  - After revert: the same sphere became `100% history-valid` with mean footprint quality
    `0.910` on the left, center, and right regions.
- The denoised-only sphere immediately recovered from the hard reprojection failure:
  - `sphere after_vs_vanilla_std_ratio: 6.75x -> 0.89x`
  - `right edge after_vs_vanilla_std_ratio: 6.67x -> 0.98x`
- After the reprojection fix, the remaining user-visible issue was no longer "missing
  history". It was specifically in the displayed-color path:
  - End-to-end sphere noise improved from the broken `6.71x` state down to `2.70x`, but the
    sphere right edge was still visibly noisy at `5.43x`.
- Conclusion:
  - The current code must **not** use the previous-surface-view-Z validation path as
    implemented here.
  - With valid surface-motion reprojection restored, the remaining sphere artifact moved
    downstream to the final displayed-color history weighting.

### Task 11: Make displayed-color history follow the stable channel
- Status: **DONE**
- Trial:
  - Changed `reblur_final_history.cs.slang` to use
    `max(diff_accum_speed, spec_accum_speed)` instead of `min(...)` for the displayed-color
    history weight.
  - Rationale: the displayed result should keep strong history if either radiance channel is
    already stable enough; the old `min(...)` path let a low specular accumulation speed leak
    fresh noisy composite back into valid-history sphere pixels.
- Commands:
  - `python3 build.py --framework macos --config Release`
  - `python3 build.py --framework macos --skip_build --run --test_case reblur_converged_history --headless true --pipeline gpu --spp 1 --use_reblur true --clear_screenshots true --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`

### Findings from Task 11
- The remaining user-visible sphere noise collapsed once displayed-color history stopped using
  the overly conservative `min(...)` weighting:
  - `E2E sphere after_vs_vanilla_std_ratio: 2.70x -> 0.91x`
  - `E2E sphere after_vs_before_std_ratio: 2.70x -> 0.91x`
  - `E2E right-edge after_vs_vanilla_std_ratio: 5.43x -> 0.97x`
  - `E2E right-edge after_vs_before_std_ratio: 6.18x -> 1.09x`
- The repo's semantic regression test is back to passing with the native macOS path:
  - `E2E FLIP = 0.1208`
  - `History cleanness = 0.81x`
  - `Noise concentration = 17.63x`
  - `Reprojection validity = 100.0%`
  - `test_converged_history.py = 14 passed, 0 failed`
- Residual note:
  - The denoised-only sphere still has a valid-history brightness drop (`sphere luma
    after/before ~= 0.82x`), but that no longer leaks into the user-visible Run 1 output after
    the displayed-color history fix.

### Task 12: Add a motion-side history shell regression that actually catches the current bug
- Status: **DONE**
- Trials:
  - Added `tests/reblur/test_motion_side_history.py`, a new camera-nudge regression that:
    - captures vanilla, full-pipeline REBLUR, denoised-only REBLUR, `TADisocclusion`, and
      `TAMaterialId`,
    - extracts visible object silhouettes from the `TAMaterialId` current-ID channel,
    - matches before/after connected components by centroid motion,
    - measures only the **history-valid motion-leading shell** of each object,
    - compares that shell's high-frequency residual against the converged vanilla reference and
      against the motion-trailing shell.
  - Did **not** add a separate primitive-ID renderer debug mode because `TAMaterialId` already
    provides a usable silhouette signal for this regression.
  - Fixed two harness issues during implementation:
    - `build.py` subprocesses inside the Python test hit the existing `git submodule update`
      sandbox problem, so the new test directly runs the built app when `--skip_build` is used.
    - archived screenshots were initially moved inside the main screenshots directory and then
      deleted by later `--clear_screenshots true` runs, so the test now archives into
      `~/Documents/sparkle/screenshots/motion_side_debug/`.
  - Wired the new test into `tests/reblur/reblur_test_suite.py` and documented it in
    `docs/REBLUR.md`.
- Commands:
  - `PYTHONPYCACHEPREFIX=/tmp/pycache python3 -m py_compile tests/reblur/test_motion_side_history.py`
  - `PYTHONPYCACHEPREFIX=/tmp/pycache python3 -m py_compile tests/reblur/reblur_test_suite.py`
  - `python3 tests/reblur/test_motion_side_history.py --framework macos --skip_build --config Release`

### Findings from Task 12
- The new regression captures the exact user-reported failure mode:
  - `Before components: 8`
  - `After components: 8`
  - `Matched components: 8`
  - All matched motion-leading shells are still marked history-valid:
    - `median leading valid fraction = 1.000` for both full-pipeline and denoised-only runs
  - But those history-valid motion-leading shells are much noisier than they should be:
    - Full pipeline:
      - `top-3 leading HF ratio mean = 6.08x`
      - `top-3 lead/trail asym mean = 6.77x`
    - Denoised-only:
      - `top-3 leading HF ratio mean = 5.49x`
      - `top-3 lead/trail asym mean = 5.49x`
- Worst current failures from the new per-object printout:
  - Full pipeline:
    - `comp 77: lead/van = 10.53x, lead/trail = 13.39x`
    - `comp 2: lead/van = 4.50x, lead/trail = 2.51x`
    - `comp 72: lead/van = 3.22x, lead/trail = 3.83x`
  - Denoised-only:
    - `comp 77: lead/van = 9.48x, lead/trail = 10.59x`
    - `comp 2: lead/van = 4.12x, lead/trail = 2.42x`
    - `comp 73: lead/van = 2.86x, lead/trail = 2.81x`
- Diagnostics now saved for visual debugging at:
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_overlay_e2e.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_excess_e2e.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_overlay_denoised.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_excess_denoised.png`
- Conclusion:
  - The remaining regression is **not** that the motion-leading shells are being classified as
    disoccluded. They are still history-valid.
  - The bug is that the history-valid motion-leading shells of multiple objects are receiving a
    noisy result anyway, in both the denoised-only and full-pipeline outputs.

### Task 13: Stop Blur/PostBlur from re-noising valid-history silhouette shells
- Status: **DONE**
- Trials:
  - Re-ran `tests/reblur/test_motion_side_history.py` as the primary acceptance test and used it
    to iterate on both the denoised-only and full-pipeline outputs.
  - Captured intermediate pass screenshots for `TAHistory`, `TemporalAccum`, `HistoryFix`,
    `Blur`, and `PostBlur`, then measured the same motion-leading shell metric against the
    converged vanilla reference.
  - Trial 1: added NRD-style view-angle skew to the Blur pass kernel in
    `reblur_blur.cs.slang`. This reduced the artifact but did not remove it robustly.
  - Trial 2: made `reblur_final_history.cs.slang` reject all partial reprojection footprints.
    This fixed the shell regression, but it regressed the old floor stability check in
    `test_converged_history.py`:
    - `Run 1 floor after/vanilla ratio = 1.107x`
    - `Run 1 floor after/before ratio = 1.286x`
  - Trial 3: narrowed `reblur_final_history.cs.slang` to a current-frame boundary-only cutoff.
    That preserved the floor but was too narrow to cover the full noisy shell band.
  - Final fix:
    - `reblur_blur.cs.slang`: kept the diffuse view-angle skew and added a 5x5
      material-boundary-band early out for Blur/PostBlur so silhouette pixels keep the already
      clean temporal result instead of being spatially re-blurred into a halo.
    - `reblur_final_history.cs.slang`: kept a 5x5 current-boundary test, but only suppresses
      displayed-color history when that boundary-band pixel also has a partial reprojection
      footprint (`!allSamplesValid`). That preserves the floor history while still refusing
      fragile final-history reuse on moving silhouette shells.
- Commands:
  - `python3 tests/reblur/test_motion_side_history.py --framework macos --skip_build --config Release`
  - `python3 tests/reblur/test_converged_history.py --framework macos --skip_build --config Release`
  - `python3 tests/reblur/reblur_pass_validation.py --framework macos --skip_build --config Release`

### Findings from Task 13
- Stage localization before the final fix showed the remaining regression was spatial, not
  reprojection:
  - `TemporalAccum`: top-3 leading HF ratio mean `1.48x`, asym `1.23x`
  - `HistoryFix`: top-3 leading HF ratio mean `1.42x`, asym `1.23x`
  - `Blur`: top-3 leading HF ratio mean `6.02x`, asym `5.00x`
  - `PostBlur`: top-3 leading HF ratio mean `6.02x`, asym `5.00x`
- The new motion-shell regression now passes in both modes:
  - Full pipeline:
    - `top-3 leading HF ratio mean = 1.08x`
    - `top-3 lead/trail asym mean = 1.12x`
  - Denoised-only:
    - `top-3 leading HF ratio mean = 1.03x`
    - `top-3 lead/trail asym mean = 1.12x`
- The original semantic nudge regression is also back to clean:
  - `E2E FLIP = 0.0972`
  - `Run 1 floor after/vanilla ratio = 0.857x`
  - `Run 1 floor after/before ratio = 0.995x`
  - `test_converged_history.py = 14 passed, 0 failed`
- The blur-path sanity check still passes after the boundary-band early out:
  - `PrePass std = 0.270733`
  - `Blur std = 0.126725`
  - `PostBlur std = 0.123725`
  - `reblur_pass_validation.py = PASS`
- Updated diagnostics remain in:
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_overlay_e2e.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_excess_e2e.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_overlay_denoised.png`
  - `~/Documents/sparkle/screenshots/motion_side_debug/diag_motion_side_excess_denoised.png`
- Conclusion:
  - The remaining bug was not missing temporal history. It was the spatial Blur/PostBlur stages
    re-filtering already-valid silhouette pixels into a one-sided halo.
  - Once Blur/PostBlur stopped touching the material-boundary shell, only a narrow
    boundary-plus-partial-footprint guard was needed in the displayed-color history path.

### Task 14: Add a semantic Run 1 e2e regression for the visible wrong-side shell
- Status: **DONE**
- Trials:
  - The user reported that the earlier shell regressions were still missing the actual visible
    e2e failure, so I added a new Python regression focused on the displayed Run 1 output instead
    of denoised-only or intermediate-pass high-frequency ratios.
  - Added `tests/reblur/test_run1_semantic_e2e.py`, which:
    - captures `vanilla`, full-pipeline `reblur_converged_history`, `TADisocclusion`, and
      `TAMaterialId`,
    - matches object silhouettes between `before` and `after`,
    - isolates only the **history-valid motion-leading shell** for each object,
    - compares that shell against the converged vanilla Run 0 image,
    - classifies shell pixels as visually wrong only when they exceed the object's own core error
      envelope, and
    - fails when a component develops a **continuous bad arc** on the history-valid shell.
  - Added `--analyze_only` to the new test so it can reuse already archived screenshots instead of
    invoking the app again. It falls back to `~/Documents/sparkle/screenshots/motion_side_debug/`
    if the new archive folder has not been populated yet.
  - Wired the new regression into `tests/reblur/reblur_test_suite.py` and documented it in
    `docs/REBLUR.md`.
- Commands:
  - `PYTHONPYCACHEPREFIX=/tmp/pycache python3 -m py_compile tests/reblur/test_run1_semantic_e2e.py`
  - `python3 tests/reblur/test_run1_semantic_e2e.py --framework macos --analyze_only`

### Findings from Task 14
- The new semantic regression fails on the current user-visible screenshots exactly where the user
  said it should:
  - analyzed components: `8`
  - failed components: `4`
  - failure reason:
    - `4 components have a visible wrong-side shell arc; max allowed is 1`
- Per-component semantic failures on the current archived e2e output:
  - `comp 73`: `bad_arc = 5 bins`, `bad_frac = 0.115`
  - `comp 76`: `bad_arc = 4 bins`, `bad_frac = 0.170`
  - `comp 72`: `bad_arc = 7 bins`, `bad_frac = 0.175`
  - `comp 74`: `bad_arc = 5 bins`, `bad_frac = 0.157`
- Large components that remain visually acceptable under this criterion:
  - `comp 1`: `bad_arc = 1 bin`
  - `comp 2`: `bad_arc = 1 bin`
- Diagnostics from the semantic e2e regression are now saved to:
  - `~/Documents/sparkle/screenshots/run1_semantic_debug/diag_run1_semantic_overlay.png`
  - `~/Documents/sparkle/screenshots/run1_semantic_debug/diag_run1_semantic_error.png`
- Conclusion:
  - We now have a solid **user-visible** acceptance test for the exact complaint: a history-valid
    motion-leading shell should not grow a visible wrong-looking arc in Run 1.
  - This regression is stricter than the earlier high-frequency shell test and currently confirms
    that the e2e issue is still unfixed.

### Task 15: Use the semantic Run 1 regression to re-localize and reduce the shell bug
- Status: **IN PROGRESS**
- Trials:
  - Re-ran both regressions on the current build:
    - `test_run1_semantic_e2e.py` now fails the fresh build with `6` bad-shell components.
    - `test_motion_side_history.py` also fails again on the fresh build (`top-3 lead/van = 6.07x`),
      so the older metric is no longer falsely passing.
  - Added a semantic per-stage sweep using the new arc detector against each stage's own
    `before -> after` pair (`PrePass`, `TemporalAccum`, `HistoryFix`, `Blur`, `PostBlur`).
  - Trial 1: in `reblur_reprojection.h.slang`, changed partial reprojection footprints from
    renormalized bilinear filtering to a dominant valid tap.
    - Effect:
      - large-shell e2e failures improved noticeably (components `1` and `2` stabilized),
      - but the small-object shell failures remained.
  - Trial 2: narrowed `reblur_final_history.cs.slang` so the current-boundary cutoff only rejects
    truly fragile partial footprints (`footprintQuality < 0.75`).
    - Effect: did not remove the remaining shell arcs.
  - Trial 3: for partial footprints whose reprojected center texel is still on the same surface,
    switched `reblur_temporal_accumulation.cs.slang`,
    `reblur_final_history.cs.slang`, and
    `reblur_stabilize_albedo.cs.slang`
    to use that exact center texel instead of any corner-derived footprint.
    - Also stopped penalizing `footprintQuality` for that center-sample path.
    - Effect:
      - large components `1` and `2` now pass the semantic e2e regression consistently,
      - remaining failures are concentrated in the smaller objects.
  - Trial 4: tightened the per-sample reprojection normal test from `dot >= 0.5` to `dot >= 0.75`.
    - Effect: no meaningful change to the remaining small-object failures.
- Commands:
  - `python3 tests/reblur/test_run1_semantic_e2e.py --framework macos --skip_build --config Release`
  - `python3 tests/reblur/test_motion_side_history.py --framework macos --skip_build --config Release`
  - semantic stage sweeps run through ad-hoc Python harnesses against:
    - `PrePass`
    - `TAHistory`
    - `TemporalAccum`
    - `HistoryFix`
    - `Blur`
    - `PostBlur`

### Findings from Task 15
- The semantic stage sweep changed the root-cause ranking:
  - `PrePass` is mostly acceptable under the semantic shell detector.
  - The user-visible wrong-side shell arc first appears in **`TemporalAccum`**, not `Blur`.
  - `HistoryFix`, `Blur`, and `PostBlur` largely inherit that already-bad shell.
- Critical observation from `TADisocclusion` on the shell pixels:
  - the history-valid motion-leading shells all report almost the same partial footprint quality:
    - `footprintQuality ~= 0.9098`
  - so the remaining issue is dominated by **partial reprojection footprints**.
- `TAHistory` split the remaining bug into two classes:
  - Large matte shells:
    - raw history is acceptable, but temporal accumulation / displayed-history weighting was still
      injecting too much current noise.
    - This is the part improved by the center-sample override and footprint-quality fix.
  - Small objects:
    - raw `TAHistory` is already wrong before TA blending:
      - `TAHistory failed components = [73, 76, 72, 75, 77, 74]`
    - so the unresolved bug is still **reprojection-side** for those objects.
- Current semantic e2e status after the successful large-shell reductions:
  - passing:
    - `comp 1`
    - `comp 2`
    - `comp 75`
  - still failing:
    - `comp 73`
    - `comp 76`
    - `comp 72`
    - `comp 77`
    - `comp 74`
  - current failure:
    - `5 components have a visible wrong-side shell arc; max allowed is 1`
- Current conclusion:
  - The earlier "Blur/PostBlur halo" diagnosis was incomplete for the fresh build.
  - The remaining regression is now localized more precisely:
    - **large-object shell failures were mostly weighting / partial-footprint reuse**
    - **small-object shell failures are still bad raw reprojection history**
