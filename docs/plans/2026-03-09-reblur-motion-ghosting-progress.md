# REBLUR Motion Ghosting Investigation Progress

## Date: 2026-03-09

## Task 1: Audit existing motion / ghosting coverage

- **Status:** DONE
- **Scope:** Compare the current REBLUR camera-motion tests against the reported bug: noisy pixels contaminating clean pixels while the camera keeps moving.

### Trials

1. Read the relevant design and prior investigation docs:
   - `docs/plans/2026-02-25-reblur-design.md`
   - `docs/plans/2026-02-27-reblur-camera-motion-design.md`
   - `docs/plans/2026-03-05-floor-noise-progress.md`
   - `docs/plans/2026-03-05-reblur-regression-progress.md`
2. Audited the current motion-focused tests and helpers:
   - `tests/reblur/MotionLuminanceTrackTest.cpp`
   - `tests/reblur/ReblurGhostingTest.cpp`
   - `tests/reblur/ReblurConvergedHistoryTest.cpp`
   - `tests/reblur/test_denoised_motion_luma.py`
   - `tests/reblur/test_motion_side_history.py`
3. Ran the existing continuous-motion luminance script directly:
   - `python3 tests/reblur/test_denoised_motion_luma.py --framework macos --skip_build`

### Findings

- The current suite has strong coverage for a single small nudge (`reblur_converged_history`) and for motion-leading shell analysis after one nudge (`test_motion_side_history.py`), but it does **not** explicitly test whether history-valid pixels get dirtier over a sequence of camera moves.
- `tests/reblur/MotionLuminanceTrackTest.cpp` no longer animates the camera. Its own file comment says the test "now runs static".
- Because of that, `tests/reblur/test_denoised_motion_luma.py` currently passes on a static-camera capture and is not a valid reproduction for the user's bug report.
- Existing repeated-motion capture support already exists in `ReblurGhostingTest.cpp`, so the shortest path is to strengthen that path plus restore real motion to the broken luminance tracker.

### Next step

- Restore real camera motion in the continuous-motion test harness.
- Add a repeated-motion contamination regression that measures whether history-valid pixels get noisier across successive nudges.

## Task 2: Strengthen motion regression coverage

- **Status:** DONE
- **Files modified:**
  - `tests/reblur/MotionLuminanceTrackTest.cpp`
  - `tests/reblur/ReblurGhostingTest.cpp`
  - `tests/reblur/test_repeated_motion_contamination.py`
  - `tests/reblur/reblur_test_suite.py`
  - `libraries/source/core/Logger.cpp`

### Trials

1. Restored real orbit-camera motion in `MotionLuminanceTrackTest.cpp`.
2. Strengthened `ReblurGhostingTest.cpp` so each nudge captures:
   - `ghosting_nudge_N_fast`
   - `ghosting_nudge_N_settled`
3. Added `tests/reblur/test_repeated_motion_contamination.py`:
   - Runs the strengthened ghosting capture in denoised, disocclusion, and material-ID modes
   - Restricts analysis to history-valid motion-leading shells
   - Compares each fast frame against its same-pose settled reference
4. Integrated the new regression into `tests/reblur/reblur_test_suite.py`.
5. While running the new coverage, hit a blocking headless crash:
   - `Logger::LogToScreen` asserted when an empty "clear this tag" message arrived before the first non-empty message for that tag.
   - Fixed by treating an unmatched empty message as a no-op.
6. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_denoised_motion_luma.py --framework macos --skip_build`

### Findings

- `MotionLuminanceTrackTest.cpp` now drives real motion again; the continuous-motion luminance script runs successfully instead of exercising a static camera.
- The strengthened ghosting harness now produces 11 screenshots per run:
  - 1 baseline
  - 5 fast-motion captures
  - 5 same-pose settled references
- The new repeated-motion regression reproduces the issue on the full denoised output:
  - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
  - Latest failing result: `top-3 leading fast/settled ratio 1.23 > 1.20`
- The failure occurs even though the history-valid fraction remains `1.000` for the analyzed shells, so the problem is not coming from explicit disocclusion rejection.

## Task 3: Stage-by-stage visual debugging investigation

- **Status:** DONE
- **Scope:** Use the same repeated-motion metric on intermediate passes to localize where the contamination enters.

### Trials

1. Ran the repeated-motion diagnostic on the full denoised output:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
2. Ran the same diagnostic on `TemporalAccum`:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass TemporalAccum`
3. Ran the same diagnostic on `PostBlur`:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass PostBlur`
4. Inspected the saved diagnostic overlays and excess-noise heatmaps under:
   - `~/Documents/sparkle/screenshots/repeated_motion_debug/`

### Findings

- **History-valid semantics:** all analyzed motion-leading shells stayed `100%` history-valid in the `TADisocclusion` captures. Clean pixels are being polluted despite reprojection claiming they are valid.
- **Per-pass statistics (2-frame fast capture):**
  - `TemporalAccum`: worst leading fast/settled ratio `1.11x`
  - `PostBlur`: worst leading fast/settled ratio `1.16x`
  - `Full`: failing ratio `1.23x`
- **Localization:** the contamination is weakest in `TemporalAccum`, larger after `PostBlur`, and worst in `Full`.
- Because the failing `Full` runs use `--reblur_no_pt_blend true`, the PT blend ramp and renderer-owned final-history reprojection are not involved.
- That isolates the main regression to the denoiser path after `PostBlur`, with temporal stabilization as the primary suspect.
- **Semantic image inspection:** the saved overlays and heatmaps show the excess concentrated on motion-leading object silhouettes and the adjacent floor region, not on disoccluded holes.

### Current conclusion

- The user-reported artifact is now covered by an automated repeated-motion regression.
- The evidence points away from motion-vector reprojection / disocclusion validity and toward post-`PostBlur` temporal stabilization retaining or amplifying dirty history on pixels that remain nominally history-valid.

## Task 4: Temporal-stabilization fix trials guided by the repeated-motion regression

- **Status:** IN PROGRESS
- **Scope:** Change the post-`PostBlur` path to stop contaminating history-valid motion-leading pixels while the camera keeps moving.

### Trials

1. Made temporal stabilization reproject its history with the same surface-valid tests used by temporal accumulation:
   - added current / previous normal-roughness and previous view-Z inputs to `reblur_temporal_stabilization.cs.slang`
   - replaced unconditional bilinear sampling with `BilinearHistorySample(...)`
   - added center-sample fallback for partial footprints
   - reset `stab_count` when the TS reprojection itself is invalid
2. Capped TS blend strength by current TA confidence and TS footprint quality instead of trusting `stab_count` alone on moving pixels.
3. Restored the `TSStabCount` diagnostic output so the pass can be inspected visually again.
4. Used the visual debug pass on the same `reblur_ghosting` sequence to inspect leading-shell blend strength under motion.
5. Trialed several stronger motion-handling variants:
   - attenuating TS blend by screen-space motion magnitude
   - attenuating `stab_count` growth by motion magnitude
   - disabling TS / stabilized albedo outright beyond a 1-pixel motion threshold
   - luminance-only TS output
6. Rebuilt and reran the repeated-motion regression after each meaningful trial:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`

### Findings

- The original TS implementation had two concrete correctness problems:
  - it sampled stabilized history with raw bilinear reprojection and no geometry/material validation
  - it could apply a very large blend on moving shell pixels because `stab_count` stayed high even when the camera kept moving
- The restored `TSStabCount` visualization showed that motion-leading shell pixels were still getting large TS blend weights (`~0.85` in the green channel) during repeated camera motion, which matched the failing regression.
- The best-performing production trial so far is:
  - geometry-aware TS reprojection
  - TS invalid-history reset
  - TS blend limited by current TA confidence, TS footprint quality, and a soft screen-space motion attenuation
- That trial materially reduced the repeated-motion contamination metric, but it did **not** fully clear the regression in a robust way:
  - best measured trial: `top-3 leading fast/settled ratio 1.21` vs threshold `1.20`
- Several stronger follow-up ideas were tested and rejected because they made the regression worse:
  - motion-attenuated `stab_count` growth
  - hard TS disable above 1 px motion
  - motion-gated stabilized albedo
  - luminance-only TS output
- Current code state is restored to the best-performing TS variant above; the issue is narrowed substantially, but the repeated-motion gate is still not convincingly green.
- Latest clean verification on the restored state:
  - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
  - result: `top-3 leading fast/settled ratio 1.25 > 1.20`
- Adjacent continuous-motion regression remains healthy on the restored state:
  - `python3 tests/reblur/test_denoised_motion_luma.py --framework macos --skip_build`
  - result: `5 passed, 0 failed`

## Task 5: Isolate `Full` into radiance TS vs stabilized albedo

- **Status:** DONE
- **Scope:** Verify whether the remaining `Full`-only contamination is actually coming from stabilized composite albedo rather than radiance temporal stabilization.

### Trials

1. Temporarily bypassed `StabilizeCompositeAlbedo(...)` in `ReblurDenoiser::Denoise(...)` so the `Full` output became:
   - radiance denoiser path as-is
   - raw current-frame albedo instead of stabilized albedo
2. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
3. Reverted the bypass immediately after measurement.

### Findings

- Bypassing stabilized albedo made the repeated-motion regression **much worse**, not better:
  - `top-3 leading fast/settled ratio 1.56 > 1.20`
  - `top-3 lead/trail asymmetry 1.47 > 1.35`
- Conclusion:
  - stabilized albedo is **not** the source of the remaining contamination
  - the unresolved bug is in the radiance temporal-stabilization path itself

## Task 6: Split the final composite and re-localize the remaining failure

- **Status:** DONE
- **Scope:** Re-check the `Full`-only failure after the TS cooldown work by exposing the composite terms directly instead of inferring them from diffuse-only debug passes.

### Trials

1. Added new `reblur_debug_pass` outputs for:
   - `CompositeDiffuse`
   - `CompositeSpecular`
   - `StabilizedAlbedo`
2. Re-ran the repeated-motion regression on each split output:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeDiffuse`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeSpecular`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass StabilizedAlbedo`
3. Trialed one blur-path idea and rejected it:
   - removed the boundary-band early-out in `reblur_blur.cs.slang`
   - rebuilt and checked `PostBlur`, `CompositeDiffuse`, and `Full`
   - restored the original blur behavior after the metric did not improve

### Findings

- The original stage-by-stage localization had a blind spot:
  - `TemporalAccum` and `PostBlur` debug passes show diffuse radiance only
  - they do **not** expose the remodulated diffuse term or the specular contribution used by `Full`
- Direct composite splits showed:
  - `CompositeDiffuse`: still failing at about `1.23x`
  - `CompositeSpecular`: very noisy in isolation (`2.31x` top-3 leading ratio), but not the dominant contributor to the final image in this scene
  - `StabilizedAlbedo`: passes cleanly (`1.09x` worst leading ratio), so the stabilized albedo history itself is not the unstable component
- That means the remaining visible failure is:
  - mostly diffuse radiance that still carries too much motion noise by the time it is remodulated
  - amplified by the albedo term in the final composite
- The blur boundary-band early-out was **not** the blocker:
  - removing it did not materially improve `PostBlur` or `Full`
  - the change was reverted

## Task 7: Fix persistent over-confidence in temporal accumulation after motion

- **Status:** DONE
- **Scope:** Stop history-valid moving shell pixels from keeping a large accumulation speed after camera motion, so later blur / stabilization stages no longer treat them as fully converged.

### Trials

1. Inspected `reblur_temporal_accumulation.cs.slang` and confirmed that history-valid pixels simply increment `accum_speed` every frame, even during significant screen-space motion.
2. Trialed a broad motion cap on TA accumulation speed:
   - cap to `2.0` when motion exceeded `1 px`
   - result: target repeated-motion regression passed, but `test_denoised_motion_luma.py` failed badly on settled quality
3. Relaxed the broad cap to `4.0`:
   - repeated-motion still passed
   - continuous-motion settled quality still failed
4. Narrowed the cap to the actual problematic reprojection case:
   - keep the `1 px` motion threshold
   - cap `accum_speed` to `2.0`
   - apply the cap **only** when the bilinear reprojection footprint is partial / fallback (`!allSamplesValid`)
5. Rebuilt and reran the verification set after the localized cap:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
   - `python3 tests/reblur/test_denoised_motion_luma.py --framework macos --skip_build`
   - `python3 tests/reblur/test_stabilization_stale_history.py --framework macos --skip_build`

### Findings

- Root cause:
  - on motion-leading shell pixels, TA could keep a high `accum_speed` even though the reprojection footprint was only partially valid
  - later stages used that inflated `accum_speed` as if the pixel were stable, shrinking their denoising footprint too aggressively on the exact frames where the camera had just moved
- The localized TA fix works:
  - latest repeated-motion verification on `Full` passes
  - latest run: worst leading fast/settled ratio `1.20x`, lead/trail asymmetry `1.02x`, history-valid median `1.000`
  - earlier clean rerun on the same implementation reached `1.14x`
- The broader motion caps were rejected because they hurt normal convergence under continuous camera motion.
- The localized cap preserves the other key motion/stability checks:
  - `test_denoised_motion_luma.py`: `5 passed, 0 failed`
  - settled quality recovered to `0.9143`
  - `test_stabilization_stale_history.py`: PASS

### Current conclusion

- The fix is not a TS-only change.
- The decisive change is reducing TA confidence for moving pixels with partial reprojection footprints, which gives downstream blur / stabilization enough room to denoise the post-motion settling frames without reintroducing the original ghosting path.
- Residual risk remains because the repeated-motion pass margin is not huge on every run, but the target regression is now green and the adjacent stabilization regressions are also green on the final state.

## Task 8: Compare the local fix against upstream NRD

- **Status:** DONE
- **Scope:** Check whether the official `external/NRD` REBLUR implementation contains guidance relevant to the motion-leading contamination bug and the local TA/TS fix.

### Trials

1. Searched the upstream NRD tree for REBLUR temporal logic and motion-confidence handling:
   - `rg --files external/NRD | rg 'REBLUR|Reblur|reblur'`
   - `rg -n "REBLUR|TemporalAccum|TemporalStabil|stabilization|accumSpeed|motion" external/NRD -S`
2. Inspected the most relevant upstream shaders:
   - `external/NRD/Shaders/REBLUR_TemporalAccumulation.cs.hlsl`
   - `external/NRD/Shaders/REBLUR_TemporalStabilization.cs.hlsl`
   - `external/NRD/Shaders/REBLUR_PostBlur.cs.hlsl`
3. Compared those paths to the local implementations:
   - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
   - `shaders/ray_trace/reblur_temporal_stabilization.cs.slang`
   - `shaders/ray_trace/reblur_blur.cs.slang`

### Findings

- Upstream NRD absolutely has useful signal for this bug.
- The strongest match is conceptual, not line-for-line:
  - NRD does **not** treat "history valid" as "history fully trusted"
  - its TA path continuously reduces effective history strength using footprint quality, pixels traveled, normal agreement, roughness, parallax, and optional history-confidence inputs
  - that aligns with the local fix, where moving pixels with partial reprojection footprints are prevented from keeping a large `accum_speed`
- NRD TS also assumes TA has already produced a meaningful history-confidence signal:
  - TS computes local moments, runs antilag, and blends stabilized history using the incoming accumulation state
  - that reinforces the final conclusion from Tasks 6-7 that the decisive fix belongs in TA state/confidence, not only in late TS clamping
- The largest remaining gap versus NRD is specular motion modeling:
  - NRD has a dedicated virtual-motion path for specular history with extra confidence tests
  - the local implementation currently uses surface-motion reprojection only
  - that is useful future guidance, but it was not required to fix the reproduced camera-motion ghosting regression in this scene
- NRD also has a more advanced post-blur weighting model based on non-linear accumulation speed and lobe-aware geometry weights.
  - useful for future robustness tuning
  - not the primary missing piece for this reproduced bug, which was over-confident TA state on moving partial-footprint pixels

### Current conclusion

- The upstream NRD comparison validates the direction of the local fix.
- It does not reveal a single missing "magic line", but it does confirm that the official algorithm is much stricter about degrading temporal confidence under imperfect motion reprojection than a simple valid/invalid test would suggest.

## Task 9: Re-tighten the repeated-motion regression and re-check TS state

- **Status:** DONE
- **Scope:** The user still observes ghosting, so strengthen the current regression to catch visible outlier shells more reliably and use visual debugging to verify whether the remaining contamination still comes from TS.

### Trials

1. Re-ran the current repeated-motion regression on the final implementation:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
2. Prototyped a TS-state debug capture for the same `reblur_ghosting` sequence:
   - `python3 build.py --framework macos --skip_build --run --test_case reblur_ghosting --headless true --clear_screenshots true --reblur_debug_pass TSStabCount`
3. Measured the failing motion-leading shells against the TS debug output:
   - compared `TSStabCount` on the same shell masks used by `test_repeated_motion_contamination.py`
   - checked normalized stabilized count and TS blend on the highest-ratio components
4. Strengthened `tests/reblur/test_repeated_motion_contamination.py`:
   - kept the existing top-3 mean gate
   - added a new worst-single-component leading-shell ratio gate
   - printed the worst component ratio per nudge for easier triage
5. Re-ran the tightened regression:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_contamination.py`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`

### Findings

- The issue is still reproducible on the current implementation.
- The strengthened regression now fails cleanly with both aggregate and outlier metrics:
  - latest run:
    - `Nudge 0: lead_fast/settled=1.18x, worst_comp=1.23x`
    - `Nudge 1: lead_fast/settled=1.14x, worst_comp=1.21x`
    - `Nudge 2: lead_fast/settled=1.11x, worst_comp=1.14x`
    - failure summary:
      - `top-3 leading fast/settled ratio 1.22 > 1.20`
      - `worst single-component leading fast/settled ratio 1.29 > 1.25`
- The new single-component gate matters:
  - the remaining artifact can be concentrated in one or two shells
  - averaging only the top-3 mean can understate a visually obvious contaminated arc
  - this matches the upstream NRD design direction, where temporal confidence is effectively local/per-pixel rather than justified by broad averages
- TS is not the active source for the remaining failure:
  - on the failing leading shells, `TSStabCount` shows `diff_blend ~= 0`
  - normalized stabilized count is only about `0.03-0.035` (roughly 2 stabilized frames out of 63), with previous stabilized count about `0.012`
  - that means the visible residual is reaching the final image before TS meaningfully blends any stabilized history

### Current conclusion

- The remaining ghosting is still real, and the old pass/fail criterion was not strict enough to flag the worst visible shell every time.
- Visual debugging narrows the remaining contamination path further:
  - it is not late TS history lock-in
  - it is more likely residual over-confidence / under-filtering earlier in TA or blur for motion-leading, history-valid shells

## Task 10: Add TA confidence visualization and a TA-localization regression

- **Status:** DONE
- **Scope:** Since TS blend is already near zero on the failing shells, expose TA accumulation state directly and check whether the contaminated motion-leading shells are still carrying large TA confidence, using the repeated-motion harness.

### Trials

1. Added a new REBLUR debug pass:
   - `TAAccumSpeed`
   - output encoding:
     - diffuse `R`: current diffuse accum normalized by max frame count
     - diffuse `G`: previous diffuse accum normalized by max frame count
     - diffuse `B`: TA footprint quality
2. Rebuilt after the debug-pass plumbing:
   - `python3 build.py --framework macos`
3. Captured the repeated-motion sequence with the new TA debug pass:
   - `python3 build.py --framework macos --skip_build --run --test_case reblur_ghosting --headless true --clear_screenshots true --reblur_debug_pass TAAccumSpeed`
4. Correlated the new TA debug output with the same history-valid motion-leading shell masks used by the contamination regression.
5. Promoted that ad hoc analysis into a dedicated script:
   - `tests/reblur/test_repeated_motion_ta_confidence.py`
6. Added the new regression to `tests/reblur/reblur_test_suite.py`.
7. Verified the new script:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_confidence.py tests/reblur/reblur_test_suite.py`
   - `python3 tests/reblur/test_repeated_motion_ta_confidence.py --framework macos --skip_build`

### Findings

- The remaining issue is still visible in TA state, not only in the final image.
- The new TA regression fails cleanly on the current implementation:
  - `Nudge 0: top-1 contaminated leading accum 8.98 > 8.00`
  - `Nudge 1: top-2 contaminated leading accum 16.03 > 8.00`
  - `Nudge 2: top-3 contaminated leading accum 21.35 > 8.00`
  - `Nudge 3: top-3 contaminated leading accum 9.38 > 8.00`
  - `Nudge 4: contaminated=3, worst_ratio=1.28x, top3_accum=7.36`
- The correlation with the contaminated shells is strong:
  - many of the shells with `lead_ratio > 1.10x` still hold `lead_accum ~= 15-25` frames
  - footprint quality on those same shells stays around `0.91`
  - example offenders from the ad hoc analysis:
    - nudge 0: `lead_ratio 1.23x`, `lead_accum 20.85`, `motion 12.85 px`
    - nudge 1: `lead_ratio 1.21x`, `lead_accum 20.58`, `motion 14.84 px`
    - nudge 2: `lead_ratio 1.14x`, `lead_accum 20.42`, `motion 14.31 px`
    - nudge 3: `lead_ratio 1.22x`, `lead_accum 19.45`, `motion 13.86 px`
- The visual TA heatmaps match the numeric result:
  - contaminated shell arcs are highlighted while the TA debug still reports strong accumulation on those regions
  - this is consistent with the upstream NRD idea that history confidence should be reduced by motion/footprint quality rather than left high simply because reprojection remains technically valid
- There are also a few extreme-ratio outliers at very large motion with low accum:
  - e.g. `lead_ratio 1.29x` with `lead_accum ~= 2.36`
  - that suggests there may be a secondary under-filtering path on large-motion shells
  - but the dominant repeated-motion failure still includes many high-accum offenders, so TA over-confidence remains a primary issue

### Current conclusion

- The investigation now has a direct semantic reproducer for the suspected root cause:
  - `test_repeated_motion_contamination.py` proves the artifact is visible
  - `test_repeated_motion_ta_confidence.py` proves many of those contaminated shells still retain large TA accumulation
- Combined with Task 9's TS result, the remaining bug window is now much narrower:
  - not TS history lock-in
  - mostly TA confidence / blur behavior on motion-leading, history-valid shells

## Task 11: Split the remaining failure into diffuse vs specular and add spec-local TA diagnostics

- **Status:** DONE
- **Scope:** The full-frame repeated-motion regression was still red, but that did not say whether the remaining contamination lived in diffuse, specular, or the composite/albedo path. Split the output, compare against NRD again, and expose specular TA state directly.

### Trials

1. Evaluated the repeated-motion contamination regression on diffuse-only output:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeDiffuse`
2. Evaluated the same regression on specular-only output:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeSpecular`
3. Added a new TA debug pass for specular accumulation:
   - `TASpecAccumSpeed`
   - composite passthrough mode `5` now outputs the specular diagnostic texture directly
4. Extended `tests/reblur/test_repeated_motion_ta_confidence.py` to support:
   - `--eval_debug_pass`
   - `--ta_channel diffuse|specular`
5. Added the specular TA-confidence regression and a dedicated suite entry for specular contamination:
   - suite entries `29` and `30`
6. Rebuilt and ran the new specular TA-confidence regression:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ta_confidence.py --framework macos --skip_build --eval_debug_pass CompositeSpecular --ta_channel specular`

### Findings

- The remaining visible bug is overwhelmingly specular:
  - diffuse-only:
    - `Nudge 0: lead_fast/settled=1.05x worst_comp=1.10x`
    - `Nudge 1: lead_fast/settled=1.08x worst_comp=1.13x`
    - `Nudge 2: lead_fast/settled=1.08x worst_comp=1.12x`
    - fail summary: `worst single-component leading fast/settled ratio 1.27 > 1.25`
  - specular-only:
    - fail summary: `top-3 leading fast/settled ratio 2.90 > 1.20`
    - `worst single-component leading fast/settled ratio 4.24 > 1.25`
    - `top-3 lead/trail asymmetry 2.66 > 1.35`
- This matches the NRD comparison:
  - upstream REBLUR spends much more logic on specular history confidence than the local implementation
  - the local implementation still relies on surface-motion reprojection only for specular
- The new specular TA-confidence regression showed that specular TA was still overconfident on the failing shells:
  - initial run:
    - `Nudge 0: top-3 contaminated leading accum 10.17 > 8.00`
    - `Nudge 1: top-2 contaminated leading accum 9.91 > 8.00`
    - `Nudge 2: top-1 contaminated leading accum 10.35 > 8.00`
  - the diagnostic `B` channel was still about `0.91`, so those shells were history-valid and still considered high-quality by the local logic

### Current conclusion

- The remaining issue is no longer best described as generic camera-motion ghosting.
- More precisely:
  - diffuse is close to acceptable, with one residual outlier shell
  - specular is still severely contaminated
  - specular TA confidence remains too high on the failing repeated-motion shells

## Task 12: Tighten specular TA confidence and probe post-blur behavior

- **Status:** DONE
- **Scope:** Use the new specular TA regression to reduce specular temporal confidence under motion, then test whether the remaining error is still TA-local or has moved to later spatial stages.

### Trials

1. Added a specular motion-history confidence term in TA, guided by NRD's surface-history-confidence idea:
   - current local rule limits specular accumulated frames under motion using:
     - motion magnitude
     - roughness-relaxed motion sensitivity
     - a low explicit motion max-accum cap
2. Rebuilt and re-ran the specular TA-confidence regression:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ta_confidence.py --framework macos --skip_build --eval_debug_pass CompositeSpecular --ta_channel specular`
3. Re-ran the specular contamination regression:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeSpecular`
4. Prototyped a specular post-blur change by removing the boundary-band bailout for specular while preserving conservative diffuse behavior.
5. Rebuilt and re-ran the same specular contamination regression after that blur change.
6. Reverted the blur change after it made the artifact worse.

### Findings

- The specular TA tightening worked in TA-space:
  - latest specular TA-confidence run:
    - `Nudge 0: contaminated=3 worst_ratio=4.21x top3_accum=7.18 top3_prev=6.99 quality=0.910`
    - `Nudge 1: contaminated=2 worst_ratio=3.09x top3_accum=7.25 top3_prev=7.05 quality=0.910`
    - `Nudge 2: contaminated=1 worst_ratio=1.38x top3_accum=8.00 top3_prev=7.78 quality=0.910`
    - overall result: `PASS`
- But the visible specular artifact did not disappear:
  - after the TA tightening, specular contamination still failed:
    - `top-3 leading fast/settled ratio 2.75 > 1.20`
    - `worst single-component leading fast/settled ratio 4.24 > 1.25`
    - `top-3 lead/trail asymmetry 2.59 > 1.35`
- That means the specular bug window narrowed again:
  - specular TA is no longer obviously too long on the failing shells
  - the remaining visible error is likely in later specular processing, not only TA
- The specular post-blur boundary-bailout removal was a dead end:
  - it worsened the specular contamination regression to:
    - `top-3 leading fast/settled ratio 3.46 > 1.20`
    - `worst single-component leading fast/settled ratio 6.40 > 1.25`
  - that trial was reverted

### Current conclusion

- The new tests and diagnostics were useful even though the bug is not fully fixed yet.
- The current best evidence is:
  - specular TA overconfidence was part of the problem and is now materially reduced
  - the dominant remaining artifact is still specular, but it is no longer explained by TA length alone
  - the next investigation should focus on the specular blur / post-blur weighting model rather than TS or diffuse TA

## Task 13: Add raw spec-history visualization and continue tightening the TA fallback

- **Status:** DONE
- **Scope:** After the specular TA-confidence gate passed, the visible specular artifact was still far too large. That suggested the remaining issue might be in the raw history content rather than only in history length. Add a spec-history debug pass, measure it directly, and keep iterating on the local no-VMB fallback.

### Trials

1. Added a new TA diagnostic pass:
   - `TASpecHistory`
   - composite passthrough mode `5` now also supports specular raw-history output
2. Rebuilt and evaluated the repeated-motion contamination metric directly on raw specular history:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass TASpecHistory`
3. Added a stronger local fallback inside TA for moving smooth specular:
   - low-roughness moving specular history now gets an extra confidence penalty even if the surface-motion footprint remains valid
   - that confidence now also raises the current-frame blend weight directly via `spec_weight = max(spec_weight, 1 - spec_history_quality)`
4. Rebuilt and re-ran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass TASpecHistory`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeSpecular`
5. Tightened the spec-motion max-accum cap once more (`2.0`) and re-ran `TASpecHistory`.
6. Reverted that last tightening after it made the raw-history metric worse, restoring the best known local state (`spec motion max accum = 3.0`).

### Findings

- The new raw-history pass was useful immediately:
  - first `TASpecHistory` run:
    - `top-3 leading fast/settled ratio 1.87 > 1.20`
    - `worst single-component leading fast/settled ratio 2.50 > 1.25`
  - this proved the remaining specular issue was already present in the reprojected history itself
- The stronger smooth-specular fallback materially improved raw history:
  - intermediate best `TASpecHistory`:
    - `top-3 leading fast/settled ratio 1.34 > 1.20`
    - `worst single-component leading fast/settled ratio 1.50 > 1.25`
    - `top-3 lead/trail asymmetry 1.58 > 1.35`
- That same change also improved the final specular output:
  - `CompositeSpecular` improved from:
    - `top-3 2.85x`, `worst 5.32x`, `asym 2.92x`
  - to:
    - `top-3 2.54x`, `worst 4.72x`, `asym 2.63x`
- The extra `spec motion max accum = 2.0` tightening was a dead end:
  - `TASpecHistory` regressed back to:
    - `top-3 1.75x`
    - `worst 2.11x`
    - `asym 2.07x`
  - that trial was reverted

### Current conclusion

- The remaining bug is still not fixed, but the failure window is smaller and better understood.
- The important new facts are:
  - raw specular history was still wrong even after the earlier TA-length fix
  - a stronger no-VMB smooth-spec fallback makes both raw spec history and final specular output better
  - the current best state still fails badly, so later specular stages are still amplifying whatever TA contamination remains

## Task 14: Add stage-local specular debug passes for intermediate REBLUR stages

- **Status:** DONE
- **Scope:** The current best checkpoint still failed badly on `CompositeSpecular`, but the remaining amplification point was ambiguous. Add specular-only variants of the intermediate stage debug passes so the repeated-motion contamination metric can be applied to each stage directly.

### Trials

1. Added new debug-pass enums:
   - `TemporalAccumSpecular`
   - `HistoryFixSpecular`
   - `BlurSpecular`
   - `PostBlurSpecular`
2. Reused the existing composite passthrough specular mode so these passes show raw `denoisedSpecular` without albedo modulation.
3. Updated `ReblurDenoiser` stage-return checks so the new specular variants exit at the same stage boundaries as their full-color counterparts.
4. Rebuilt:
   - `python3 build.py --framework macos`

### Findings

- The stage-local specular instrumentation built cleanly.
- No shader behavior changed yet; this task only added observability for the next regression sweep.
- The new passes are now available for serial repeated-motion runs:
  - `TemporalAccumSpecular`
  - `HistoryFixSpecular`
  - `BlurSpecular`
  - `PostBlurSpecular`

### Current conclusion

- The investigation can now localize the remaining specular amplification to a specific post-TA stage instead of inferring it indirectly from `TASpecHistory` versus `CompositeSpecular`.

## Task 15: Sweep stage-local specular contamination and compare the actual captures

- **Status:** DONE
- **Scope:** Use the new specular-only stage passes with the repeated-motion regression, then sanity-check the result visually against the archived screenshots before changing more shader logic.

### Trials

1. Rebuilt with the new stage-local specular debug passes:
   - `python3 build.py --framework macos`
2. Ran the repeated-motion contamination regression serially on:
   - `TemporalAccumSpecular`
   - `HistoryFixSpecular`
   - `BlurSpecular`
   - `PostBlurSpecular`
3. Cross-checked NRD while interpreting the results:
   - `REBLUR_HistoryFix.cs.hlsl`
   - `REBLUR_TemporalAccumulation.cs.hlsl`
4. Tried two local `HistoryFix` fixes:
   - channel-specific spec/diff gating plus roughness / hit-distance shaping
   - stronger low-roughness suppression of reconstructed specular
5. Rebuilt and re-ran `HistoryFixSpecular` after each trial.
6. Reverted the low-roughness suppression trial after it made the metric worse.
7. Ran a clean diagnostic trial that bypassed specular neighbor contribution inside `HistoryFix`, rebuilt, and re-ran:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass HistoryFixSpecular`
8. Preserved both `TemporalAccumSpecular` and `HistoryFixSpecular` capture sets and compared the actual fast/settled screenshots plus mask images directly.

### Findings

- The raw stage-local ratios were:
  - `TemporalAccumSpecular`: `top-3 1.96x`, `worst 3.04x`, `asym 2.16x`
  - `HistoryFixSpecular`: `top-3 3.57x`, `worst 5.42x`, `asym 1.91x`
  - `BlurSpecular`: `top-3 3.57x`, `worst 5.43x`, `asym 1.91x`
  - `PostBlurSpecular`: `top-3 3.57x`, `worst 5.43x`, `asym 1.91x`
- The first local `HistoryFix` shaping trial did not move the metric materially.
- The stronger low-roughness suppression trial made `HistoryFixSpecular` worse:
  - `top-3 3.81x`
  - `worst 6.32x`
  - `asym 2.01x`
  - that trial was reverted
- The key visual/debugging result is that the stage-local ratios were misleading if interpreted naively:
  - the `TemporalAccumSpecular` fast frame and the `HistoryFixSpecular` fast frame are nearly identical by eye
  - direct image diff confirmed that:
    - TA fast vs HF fast mean absolute difference was only about `5.4e-06`
    - disocclusion and material-mask images were bit-identical across the two runs
  - the big difference was in the settled reference:
    - TA settled vs HF settled mean absolute difference was about `1.42e-03`
    - about `1.17%` of pixels differed by more than one 8-bit code value
- That means the large `HistoryFixSpecular` ratio is mostly because the same-stage settled reference becomes cleaner than TA’s settled reference, not because `HistoryFix` suddenly made the fast frame much dirtier.
- The specular-neighbor-bypass diagnostic also stayed around `3.59x`, which is consistent with the finding above:
  - it did not prove that `HistoryFix` neighbor reconstruction is the dominant remaining bug
  - it proved that this particular fast/settled ratio is not a reliable cross-stage localization metric by itself

### Current conclusion

- The current repeated-motion regression is still valid for end-to-end `CompositeSpecular` and `Full`.
- But using the same fast/settled ratio to compare different intermediate stages is confounded by stage-specific settling strength.
- The next useful investigation should use a stage-local reference that stays fixed across stages, for example:
  - compare each stage’s fast frame against a common settled target
  - or add a dedicated stage-local debug output for the actual per-pixel reuse / reconstruction amount instead of inferring from stage output alone

## Task 16: Add a fixed-reference stage-fast test and rerun the localization

- **Status:** DONE
- **Scope:** The previous stage-local contamination metric was confounded by different settled references. Add a new test that compares fast frames directly between stages on the same leading-shell pixels.

### Trials

1. Added a new diagnostic test:
   - `tests/reblur/test_repeated_motion_stage_fast_delta.py`
2. Verified it parses:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_stage_fast_delta.py`
3. Ran the new fast-frame comparison on the current checkpoint:
   - `TemporalAccumSpecular` vs `HistoryFixSpecular`
   - `HistoryFixSpecular` vs `BlurSpecular`
   - `BlurSpecular` vs `PostBlurSpecular`
4. Re-applied an NRD-inspired `HistoryFix` specular shaping pass using the new metric:
   - separate diffuse/spec gating
   - roughness-scaled spec stride
   - spec hit-distance rejection
5. Rebuilt and re-ran:
   - `TemporalAccumSpecular` vs `HistoryFixSpecular`
   - `BlurSpecular` vs `PostBlurSpecular`
   - `CompositeSpecular`
   - `Full`

### Findings

- The new fast-frame test resolved the earlier ambiguity immediately:
  - before the `HistoryFix` change:
    - `HistoryFixSpecular` vs `TemporalAccumSpecular`: `top-3 1.23x`, `worst 1.27x`, `asym 1.20x`
    - `BlurSpecular` vs `HistoryFixSpecular`: `1.00x` across all nudges
    - `PostBlurSpecular` vs `BlurSpecular`: `1.24x`, `1.30x`, `1.20x`
  - so the real fast-frame amplifiers were:
    - `HistoryFix`
    - `PostBlur`
  - while `Blur` itself was not amplifying the fast frame
- The NRD-style `HistoryFix` shaping helped materially under the new metric:
  - after the change:
    - `HistoryFixSpecular` vs `TemporalAccumSpecular` dropped to about:
      - `Nudge 0: 1.01x`
      - `Nudge 1: 1.02x`
      - `Nudge 2: 1.01x`
    - but still failed overall on a worst component:
      - `top-3 1.08x`
      - `worst 1.21x`
- The same checkpoint removed the `PostBlur` fast-frame amplification:
  - `PostBlurSpecular` vs `BlurSpecular` became `1.00x` across all nudges
- End-to-end impact on the current checkpoint:
  - `CompositeSpecular` is still very red:
    - `top-3 2.87x`
    - `worst 5.36x`
    - `asym 2.94x`
  - `Full` improved a lot and is now close to passing:
    - `Nudge 0: 1.11x`
    - `Nudge 1: 1.08x`
    - `Nudge 2: 1.06x`
    - fail only on `worst single-component leading fast/settled ratio 1.28 > 1.25`

### Current conclusion

- The new fast-delta test is a better localization tool than the old cross-stage fast/settled ratio.
- The current code reduced real fast-frame amplification in both:
  - `HistoryFix`
  - `PostBlur`
- But the isolated specular output is still far from fixed, while the full image is now only slightly over the tightened threshold.
- The remaining work should focus on why the isolated specular settled reference is still so unstable even after the fast-frame amplifiers were reduced.

## Task 17: Re-check the latest TA / TS motion-confidence ideas against the new stage-fast metric

- **Status:** IN PROGRESS
- **Scope:** Continue from the near-pass checkpoint and use the repeated-motion regressions to evaluate new specular motion-confidence ideas without losing comparability.

### Trials

1. Tried to run screenshot-based regressions in parallel:
   - `test_repeated_motion_stage_fast_delta.py`
   - `test_repeated_motion_contamination.py --eval_debug_pass Full`
2. Found that both tests clear and reuse the same screenshot directory, so the parallel run corrupted the artifact set and produced unusable results.
3. Re-ran the regressions serially on the TA hit-distance-confidence trial:
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass TASpecHistory --compare_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
4. Isolated `TemporalAccumSpecular` directly with:
   - `python3 build.py --framework macos --skip_build --run --test_case reblur_ghosting --headless true --clear_screenshots true --reblur_no_pt_blend true --reblur_debug_pass TemporalAccumSpecular`
   to confirm the missing archived files were not a renderer-side screenshot failure.
5. Compared against NRD again:
   - `external/NRD/Shaders/REBLUR_TemporalStabilization.cs.hlsl`
   - `external/NRD/Shaders/REBLUR_Common.hlsli`
6. Reverted the TA hit-distance penalty after it failed to improve the visible gate and regressed the stage-local specular fast delta.
7. Added an NRD-inspired spec-only TS brake:
   - roughness-dependent spec stabilization acceleration
   - hit-distance-dependent blend attenuation during motion
8. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass CompositeSpecular`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass CompositeSpecular`
9. Tried a stronger follow-up TS motion brake that starts below `1 px` of screen travel, rebuilt, and reran the same checks.
10. Reverted that stronger low-motion TS tweak after it made isolated `CompositeSpecular` substantially worse while leaving `Full` unchanged.

### Findings

- The screenshot-based visual regressions are **not safe to run in parallel** today because each invocation uses `--clear_screenshots true` on the same directory.
- The TA hit-distance proxy was not the right fix:
  - `TASpecHistory -> TemporalAccumSpecular` still failed at about `top-3 1.17x`, `worst 1.44x`
  - `Full` stayed at `worst single-component leading fast/settled ratio 1.29 > 1.25`
  - it also regressed `PostBlurSpecular -> CompositeSpecular` fast delta to about `top-3 1.10x`, `worst 1.23x`
- NRD comparison exposed a more relevant missing piece in TS:
  - specular stabilization history weight is explicitly reduced by a roughness-dependent acceleration term
  - our TS path previously lacked an equivalent spec-only brake
- The lighter TS spec brake is directionally useful but not sufficient:
  - `PostBlurSpecular -> CompositeSpecular` fast delta improved to about `top-3 1.10x`, `worst 1.12x`
  - isolated `CompositeSpecular` improved from about `2.87x / 5.36x` to about `2.59x / 4.82x`
  - `Full` did **not** improve and remained at the near-pass checkpoint:
    - `Nudge 0: 1.11x`
    - `Nudge 1: 1.09x`
    - `Nudge 2: 1.06x`
    - fail still on `worst single-component leading fast/settled ratio 1.28 > 1.25`
- The stronger low-motion TS brake was a dead end:
  - it nearly cleared the stage-local fast-delta gate (`worst 1.08x`)
  - but isolated `CompositeSpecular` got much worse again (`2.95x / 5.60x`)
  - `Full` still did not move
  - that trial was reverted

### Current conclusion

- The remaining visible failure is still real and still small in `Full`, but the latest useful evidence says:
  - the TA hit-distance proxy is not the right lever
  - a spec-only TS brake helps isolated specular somewhat
  - stronger low-motion TS suppression can improve fast-frame metrics while making same-stage specular settling worse
- The next useful step should measure and localize **settled-output instability** directly instead of relying only on fast-frame amplification:
  - add a stage-local settled-delta diagnostic, or
  - compare each stage against a fixed settled reference rather than only using same-stage fast/settled ratios

## Task 18: Add a stage-local settled-delta diagnostic and use it on `PostBlurSpecular -> CompositeSpecular`

- **Status:** DONE
- **Scope:** The previous investigation still lacked a direct measure of same-stage settling drift. Add a new repeated-motion test that compares settled outputs between stages on the same history-valid leading shells.

### Trials

1. Added a new diagnostic script:
   - `tests/reblur/test_repeated_motion_stage_settled_delta.py`
2. Verified it parses:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_stage_settled_delta.py`
3. Ran it on the current retained checkpoint:
   - `python3 tests/reblur/test_repeated_motion_stage_settled_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass CompositeSpecular`
4. Tightened the script output so it reports every nudge plus the worst-case summary instead of aborting on the first threshold breach.
5. Re-ran the same diagnostic after that output fix.

### Findings

- The new test fills the exact gap left by the fast-delta diagnostic:
  - it uses the same repeated-motion captures, disocclusion mask, and material shell extraction
  - but compares `settled` outputs instead of `fast` outputs
- On `PostBlurSpecular -> CompositeSpecular`, the absolute settled-output drift is **small**:
  - `Nudge 0: lead base/compare settled gain 1.02x`
  - `Nudge 1: 1.03x`
  - `Nudge 2: 1.02x`
  - worst single component: `1.06x`
- But the settled-output drift is **highly asymmetric** on the motion-leading side:
  - `Nudge 0: settled lead/trail asymmetry 1.95x`
  - `Nudge 1: 1.91x`
  - `Nudge 4: 1.37x`
  - worst asymmetry overall: `1.95x`
- That means the remaining `CompositeSpecular` issue is not simply:
  - “TS makes everything much cleaner after settling”
- It is more specifically:
  - `CompositeSpecular` still changes the **leading shell** much more than the trailing shell even after both stages are allowed to settle
  - but the compare stage is not globally converging to a dramatically cleaner reference than `PostBlurSpecular`
- This is useful because it narrows the remaining bug further:
  - the unresolved behavior is still motion-leading and history-shaped
  - but it is not dominated by a large absolute settled-output denoising gain between `PostBlurSpecular` and `CompositeSpecular`

### Current conclusion

- The new settled-delta diagnostic is worth keeping as a permanent investigation tool.
- Combined with the existing fast-delta test, the current picture is:
  - fast-frame amplification from `PostBlurSpecular -> CompositeSpecular` is now modest but still present
  - same-stage settled drift is small in magnitude, but strongly leading/trailing asymmetric
- The next fix pass should target that **leading-side asymmetry** directly rather than chasing large global noise-reduction differences between the stages.

## Task 19: Add TS-state diagnostics and test whether the remaining asymmetry is really in stabilization state

- **Status:** DONE
- **Scope:** The settled-delta result narrowed the bug to the TS side, but it still did not say which TS state variable stayed asymmetric. Add explicit TS debug output and a repeated-motion regression for settled specular TS state.

### Trials

1. Added `TSSpecBlend` debug plumbing through:
   - `libraries/include/renderer/RenderConfig.h`
   - `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`
   - `libraries/source/renderer/renderer/GPURenderer.cpp`
   - `shaders/ray_trace/reblur_temporal_stabilization.cs.slang`
2. Added a new diagnostic script:
   - `tests/reblur/test_repeated_motion_ts_spec_blend.py`
3. Verified the raw debug pass directly:
   - `python3 build.py --framework macos --skip_build --run --test_case reblur_ghosting --headless true --clear_screenshots true --reblur_no_pt_blend true --reblur_debug_pass TSSpecBlend`
4. Ran the new TS-state regression:
   - `python3 tests/reblur/test_repeated_motion_ts_spec_blend.py --framework macos --skip_build`
5. Tried a boundary-motion spec antilag cap in TS:
   - added a local material-boundary scan
   - capped `spec_antilag` for moving smooth specular near boundaries
6. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_blend.py --framework macos --skip_build`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
7. Reverted that boundary-motion cap after it failed to move the TS-state regression and left the image-level gate effectively unchanged.

### Findings

- The raw `TSSpecBlend` pass captured correctly and is worth keeping as a durable TS visual-debug tool.
- The settled TS-state regression shows the remaining asymmetry is **not** in TS blend or footprint:
  - settled `spec_blend` on the motion-leading shells is effectively `0.00`
  - settled footprint quality is symmetric at about `0.91 / 0.91`
- The asymmetry is in **spec antilag** itself:
  - `Nudge 0: antilag lead/trail 1.23x`
  - `Nudge 1: 1.20x`
  - `Nudge 2: 1.21x`
  - `Nudge 3: 1.19x`
  - worst overall: `1.23x`
- The ad hoc boundary-motion cap was the wrong fix:
  - settled TS antilag asymmetry stayed at about `1.23x`
  - `Full` stayed effectively flat and still failed at `worst single-component leading fast/settled ratio 1.28 > 1.25`

### Current conclusion

- The remaining specular failure is still entering the final image through TS-shaped state, but the useful signal is now:
  - high leading-side `spec_antilag`
  - not high settled `spec_blend`
  - not asymmetric TS footprint quality
- The next useful debug step should split `spec_antilag` into its actual inputs rather than continue trying scalar caps.

## Task 20: Split TS specular antilag into direct inputs, compare against NRD, and test the missing quad idea

- **Status:** DONE
- **Scope:** Use NRD’s `ComputeAntilag` as the reference model, then expose the TS specular divergence/confidence inputs directly so the next fix can target the dominant term instead of another heuristic.

### Trials

1. Compared the current TS antilag path against:
   - `external/NRD/Shaders/REBLUR_TemporalStabilization.cs.hlsl`
   - `external/NRD/Shaders/REBLUR_Common.hlsli`
2. Tried an NRD-style footprint-weighted antilag confidence term in TS:
   - `spec_antilag` and `diff_antilag` used `stab_footprint_quality * accum_speed`
3. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_blend.py --framework macos --skip_build`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
4. Reverted that footprint-weighted trial after it only moved TS asymmetry from `1.23x` to `1.22x` and made `Full` slightly worse (`1.29 > 1.25`).
5. Added a new TS debug pass:
   - `TSSpecAntilagInputs`
   - outputs: divergence `d`, incoming spec confidence, outgoing spec confidence
6. Added a new repeated-motion diagnostic:
   - `tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py`
7. Verified and ran it:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py`
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py --framework macos --skip_build`
8. Tried NRD’s missing quad-adaptation idea by propagating higher local divergence through `QuadReadAcrossX/Y` before computing TS antilag.
9. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py --framework macos --skip_build`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
10. Reverted the quad trial after it only nudged `Full` from `1.28` to `1.26`, still failed the gate, and introduced a new quad-capability shader warning.
11. Rebuilt cleanly again on the retained checkpoint with the new diagnostics preserved:
   - `python3 build.py --framework macos`

### Findings

- The NRD comparison was useful in two ways:
  - it confirmed that our current TS antilag input model is missing both footprint weighting and quad adaptation
  - it also clarified that both are refinements to the same underlying `ComputeAntilag` signal, not independent end-stage tricks
- The footprint-weighted trial was not strong enough to matter:
  - settled TS antilag asymmetry only changed from `1.23x` to `1.22x`
  - `Full` regressed slightly to `worst single-component leading fast/settled ratio 1.29 > 1.25`
- The new `TSSpecAntilagInputs` regression is the most useful new result in this block:
  - `Nudge 0: lead_d=0.137 trail_d=0.272 d_lt=0.19x lead_in=0.37 trail_in=0.30 in_lt=1.42x lead_out=0.41 trail_out=0.39 out_lt=1.35x`
  - `Nudge 1: d_lt=0.24x in_lt=1.28x out_lt=1.11x`
  - `Nudge 2: d_lt=0.13x in_lt=1.26x out_lt=1.15x`
  - `Nudge 3: d_lt=0.12x in_lt=1.27x out_lt=1.12x`
  - worst overall:
    - divergence lead/trail ratio: `0.12x`
    - incoming confidence asymmetry: `1.42x`
    - outgoing confidence asymmetry: `1.35x`
- This is more specific than the earlier `TSSpecBlend` result:
  - the motion-leading side enters TS with **more** spec confidence already
  - TS also sees **far less** normalized divergence there than on the trailing side
  - so the remaining bug is not just “TS keeps too much history”; it is “TS receives overconfident incoming spec history and also under-detects divergence on the leading side”
- The NRD-style quad adaptation was directionally correct but too weak on its own:
  - the new input diagnostic moved only slightly:
    - `d_lt` improved from about `0.12x` to `0.14x`
    - incoming/outgoing confidence asymmetry barely changed
  - `Full` improved only marginally to `1.26 > 1.25`
  - because it was still failing and added shader capability cost, that trial was reverted

### Current conclusion

- The most useful result from this task is the new TS-input diagnostic, not the temporary quad trial.
- The remaining specular ghosting is now localized more concretely than before:
  - TS divergence detection is much weaker on the leading side
  - but the larger asymmetry is already present in **incoming spec confidence** before TS finishes its work
- The next fix pass should target the source of that incoming leading-side confidence asymmetry:
  - most likely the specular path feeding TS (`PostBlurSpecular` / incoming accum confidence)
  - and only secondarily TS’s local divergence response

## Task 21: Fix the debug capture path and correct the TS-confidence interpretation

- **Status:** DONE
- **Scope:** The first `TSSpecAntilagInputs` screenshots were being analyzed after ACES tone mapping, which made the numeric channels unsuitable for raw debugging. Fix the capture path for REBLUR numeric diagnostics and re-run the TS / TA state tests on raw values.

### Trials

1. Traced the screenshot path through:
   - `libraries/source/renderer/renderer/GPURenderer.cpp`
   - `libraries/source/renderer/pass/ToneMappingPass.cpp`
   - `shaders/screen/tone_mapping.ps.slang`
2. Confirmed screenshots are normally captured **after** ACES tone mapping.
3. Added tone-mapping bypass for numeric REBLUR diagnostics:
   - `TADisocclusion`
   - `TAMotionVector`
   - `TADepth`
   - `TAHistory`
   - `TASpecHistory`
   - `TAMaterialId`
   - `TAAccumSpeed`
   - `TASpecAccumSpeed`
   - `TSStabCount`
   - `TSSpecBlend`
   - `TSSpecAntilagInputs`
4. Corrected `TSSpecAntilagInputs` itself so G/B export actual confidence (`1 - GetNonLinearAccumSpeed`) instead of the inverse reuse-weight.
5. Added a new settled TA regression:
   - `tests/reblur/test_repeated_motion_ta_spec_settled_asymmetry.py`
6. Verified and ran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_spec_settled_asymmetry.py`
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ta_spec_settled_asymmetry.py --framework macos --skip_build`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py --framework macos --skip_build`

### Findings

- The earlier TS-confidence interpretation was wrong because the screenshots were not raw.
- After bypassing ACES, the settled TA asymmetry test shows the TA-only path is largely fine on the retained checkpoint:
  - `Nudge 0: lead_cur=8.72 trail_cur=8.40 cur_lt=1.07x`
  - `Nudge 1: cur_lt=1.08x`
  - worst settled current accum asymmetry: `1.08x`
  - history-quality asymmetry: `1.00x`
  - result: `PASS`
- After the raw-capture fix, `TSSpecAntilagInputs` says something very different from the old tone-mapped version:
  - incoming confidence asymmetry is basically gone:
    - worst incoming asymmetry: `1.00x`
    - worst outgoing asymmetry: `1.03x`
  - the unresolved failure is almost entirely in **divergence**:
    - `Nudge 0: d_lt=0.17x`
    - `Nudge 1: 0.20x`
    - `Nudge 2: 0.13x`
    - `Nudge 3: 0.17x`
    - worst overall: `0.13x`
- This invalidates the previous “incoming confidence asymmetry is the main remaining lever” conclusion.

### Current conclusion

- The durable result from this task is the raw-debug capture path.
- On raw values, the remaining specular problem is now much clearer:
  - TA-only settled confidence is mostly symmetric
  - TS incoming/outgoing confidence is also mostly symmetric
  - TS still sees **far less divergence on the motion-leading side**
- The next fix pass should target why TS’s local specular divergence signal is so weak there, rather than further tuning confidence writeback.

## Task 22: Separate TS clamp-band width from raw history delta, then test direct TS tightening ideas

- **Status:** DONE
- **Scope:** The raw `TSSpecAntilagInputs` result still did not explain *why* TS divergence stayed so weak. Add a second TS diagnostic that separates raw history delta from allowed clamp-band width, then test whether direct TS clamp tightening is enough.

### Trials

1. Added a new TS debug pass:
   - `TSSpecClampInputs`
   - outputs:
     - history delta to local mean
     - relative clamp-band width
     - resulting divergence term
2. Added a new repeated-motion regression:
   - `tests/reblur/test_repeated_motion_ts_spec_clamp_inputs.py`
3. Verified and ran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ts_spec_clamp_inputs.py`
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_clamp_inputs.py --framework macos --skip_build`
4. Tried an NRD-inspired blur/post-blur quad-neighbor stabilization idea in:
   - `shaders/ray_trace/reblur_blur.cs.slang`
   to see if making the current post-blur neighborhood more conservative would raise TS divergence.
5. Rebuilt and reran the raw TS-input regression:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py --framework macos --skip_build`
6. Reverted that blur quad trial after it left the raw TS divergence metric effectively unchanged and added a new quad-capability shader warning.
7. Tried direct TS clamp-band tightening for moving smooth specular by scaling `spec_sigma_scale`.
8. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ts_spec_clamp_inputs.py --framework macos --skip_build`
9. Reverted that TS clamp-tightening trial after it also failed to materially move the clamp-input regression.
10. Rebuilt cleanly again on the retained checkpoint:
    - `python3 build.py --framework macos`

### Findings

- The new clamp-input regression is useful and exposes two separate asymmetries:
  - **smaller raw history delta** on the leading side
  - **somewhat wider clamp band** on the leading side
- On the retained checkpoint:
  - `Nudge 0: delta_lt=0.42x band_lt=1.07x d_lt=0.46x`
  - `Nudge 1: delta_lt=0.45x band_lt=1.06x d_lt=0.42x`
  - `Nudge 2: delta_lt=0.39x band_lt=1.22x d_lt=0.32x`
  - `Nudge 3: delta_lt=0.42x band_lt=1.14x d_lt=0.34x`
  - worst overall:
    - history-delta ratio: `0.39x`
    - clamp-band asymmetry: `1.22x`
    - divergence ratio: `0.32x`
- The blur/post-blur quad-neighbor trial was not enough:
  - raw TS divergence stayed effectively unchanged
  - it also introduced the same quad-capability warning seen in the earlier TS quad trial
- Direct TS clamp tightening was also not enough:
  - the history-delta asymmetry remained the dominant term
  - clamp-band width changed only slightly
  - the diagnostic still failed by a wide margin

### Current conclusion

- The current post-blur / TS neighborhood on the motion-leading side is not just “allowing too much band”; it is already **closer to the contaminated local mean** before clamping.
- That makes the remaining root cause more upstream than the recent TS-only tuning attempts:
  - TS clamp tuning alone is not enough
  - the leading-side current specular neighborhood feeding TS is already too contaminated
- The next fix pass should target the signal that feeds TS’s moments on the leading side rather than another writeback / clamp scalar in TS itself.

## Task 23: Add a pre-composite stabilized-specular debug path, then re-localize the remaining fast-frame contamination

- **Status:** DONE
- **Scope:** The earlier `PostBlurSpecular -> CompositeSpecular` stage delta looked like a real fast-frame amplifier, but the screenshots still mixed in final composite semantics. Add a true pre-composite stabilized-specular debug path, then rerun the repeated-motion regressions against it before touching shader logic again.

### Trials

1. Added a new debug pass:
   - `StabilizedSpecular`
   - implemented as a passthrough of `denoisedSpecular` **after TS** and **before** final composite background handling
2. Wired that pass through:
   - `libraries/include/renderer/RenderConfig.h`
   - `libraries/source/renderer/renderer/GPURenderer.cpp`
3. Rebuilt:
   - `python3 build.py --framework macos`
4. Ran the raw repeated-motion contamination regression on:
   - `HistoryFixSpecular`
   - `PostBlurSpecular`
   - `CompositeSpecular`
   - `StabilizedSpecular`
5. Ran serial stage-local regressions:
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass CompositeSpecular`
   - `python3 tests/reblur/test_repeated_motion_stage_settled_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass CompositeSpecular`
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass StabilizedSpecular`
   - `python3 tests/reblur/test_repeated_motion_stage_settled_delta.py --framework macos --skip_build --base_debug_pass PostBlurSpecular --compare_debug_pass StabilizedSpecular`
6. Inspected the saved screenshots directly to compare `PostBlurSpecular`, `CompositeSpecular`, and `StabilizedSpecular`.
7. Mistakenly launched two screenshot regressions in parallel once, noticed the shared screenshot-directory conflict immediately, killed both runs, and reran serially. No results from the parallel attempt were kept.

### Findings

- Raw same-stage contamination is still severe before TS settles:
  - `HistoryFixSpecular`: `top-3 2.01x`, `worst 2.88x`, `lead/trail 1.64x`
  - `PostBlurSpecular`: `top-3 2.03x`, `worst 2.90x`, `lead/trail 1.64x`
  - `CompositeSpecular`: `top-3 2.48x`, `worst 4.14x`, `lead/trail 2.63x`
- The new `StabilizedSpecular` pass shows the big `CompositeSpecular` jump is mostly not a final-composite background artifact:
  - `StabilizedSpecular`: `top-3 2.44x`, `worst 3.99x`, `lead/trail 2.57x`
  - so the remaining isolated-specular failure is already present **before** final composite.
- But the new stage-local split changes the interpretation of where fast-frame contamination is being injected:
  - `PostBlurSpecular -> StabilizedSpecular` fast delta: `PASS`
    - worst leading compare/base fast ratio: `1.03x`
  - `PostBlurSpecular -> StabilizedSpecular` settled delta: `FAIL`
    - worst leading settled gain: `1.13x`
    - worst settled asymmetry: `1.44x`
- That means TS is **not** materially amplifying the fast frame anymore on the retained checkpoint. It mostly changes how the same pose settles later.
- The screenshots also exposed why `PostBlurSpecular -> CompositeSpecular` had looked so suspicious:
  - `PostBlurSpecular` is a raw specular passthrough
  - `CompositeSpecular` still preserves already-rendered background semantics on sky pixels
  - that made the old `CompositeSpecular` isolation metric directionally useful, but not the cleanest stage-local target

### Current conclusion

- The durable result from this task is the new `StabilizedSpecular` debug pass.
- With that pass in place, the remaining visible specular ghosting is better localized:
  - the big isolated-specular failure survives **before** final composite
  - TS does **not** significantly amplify the fast frame
  - the immediate moving-camera contamination must therefore enter **upstream of TS**
- The next task should re-check the raw TA boundary against `TASpecHistory`, because `HistoryFix`, `PostBlur`, and TS are no longer credible fast-frame injectors on the retained checkpoint.

## Task 24: Re-check the raw TA boundary against NRD, then discard two bad TA confidence trials

- **Status:** DONE
- **Scope:** Use the new `StabilizedSpecular` split to re-test the earliest specular stages, compare that result against NRD’s continuous surface-history confidence idea, and try the smallest local TA variants that could reduce the fast-frame injection without re-opening the later TS problem.

### Trials

1. Ran raw contamination on:
   - `TemporalAccumSpecular`
   - `TASpecHistory`
2. Ran the key stage-local fast delta:
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass TemporalAccumSpecular --compare_debug_pass HistoryFixSpecular`
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass TASpecHistory --compare_debug_pass TemporalAccumSpecular`
3. Re-read the local TA shader and the official NRD TA implementation, focusing on:
   - specular surface-history confidence
   - max-frame limiting
   - continuous motion/parallax confidence rather than binary `historyValid`
4. Trial A:
   - added an NRD-inspired hit-distance-sensitive specular confidence term under the existing `motion_pixels > 1.0` TA motion gate
5. Rebuilt and reran:
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_stage_fast_delta.py --framework macos --skip_build --base_debug_pass TASpecHistory --compare_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
6. Trial B:
   - removed the hard TA motion gate for that new spec confidence so it would also affect the low-motion regime
7. Rebuilt and reran the same focused TA regressions.
8. Trial C:
   - reverted Trial B
   - instead added a partial-footprint-only spec confidence penalty for `!allSamplesValid`
9. Rebuilt and reran the same focused TA regressions again.
10. Reverted Trials A, B, and C after they all regressed the broader TA contamination gate.

### Findings

- The new `StabilizedSpecular` split made the earliest fast-frame localization unambiguous:
  - `TemporalAccumSpecular -> HistoryFixSpecular` fast delta: `PASS`
    - worst leading compare/base fast ratio: `1.01x`
  - `TASpecHistory -> TemporalAccumSpecular` fast delta: `FAIL`
    - baseline retained checkpoint: `top-3 1.13x`, `worst 1.34x`
- That means the remaining fast-frame specular contamination is still being injected at the **TA blend itself**, not in `HistoryFix`, `PostBlur`, TS, or final composite.
- Same-stage raw contamination also supports that ordering:
  - `TASpecHistory`: `top-3 1.49x`, `worst 1.75x`, `lead/trail 1.97x`
  - `TemporalAccumSpecular`: `top-3 1.62x`, `worst 2.03x`, `lead/trail 1.87x`
- Trial A was too weak:
  - `TASpecHistory -> TemporalAccumSpecular` stayed at `top-3 1.13x`, `worst 1.34x`
  - `TemporalAccumSpecular` contamination stayed at `top-3 1.62x`, `worst 2.03x`
- Trial B was directionally interesting on the fast-delta metric but unacceptable overall:
  - `TASpecHistory -> TemporalAccumSpecular` improved to `top-3 1.10x`, `worst 1.27x`
  - but same-stage `TemporalAccumSpecular` contamination blew up to about `top-3 2.78x`, `worst 4.77x`
- Trial C was worse still:
  - `TASpecHistory -> TemporalAccumSpecular` regressed to about `top-3 1.51x`, `worst 1.65x`
- All three TA confidence variants were reverted.

### Current conclusion

- The useful retained result from this task is not a shader fix; it is a cleaner localization:
  - the remaining fast-frame issue is **specular TA blend**
  - the later stages mostly affect how the reference settles
- The official NRD reference is still useful here because it confirms the shape of the missing signal:
  - continuous surface-history confidence
  - motion/parallax-aware specular confidence
  - more than a binary valid/invalid reprojection
- But the quick local analogues tried here were too blunt. The next TA pass needs a more faithful confidence signal, likely based on the actual low-parallax / high-footprint-quality shells that still fail the repeated-motion regression, rather than another broad accum-speed cap.

## Task 25: Measure the actual TA motion / confidence regime of the contaminated shells before attempting another fix

- **Status:** DONE
- **Scope:** Stop changing blend logic and collect direct evidence from the contaminated shells themselves. The questions were:
  - is the remaining repro actually a low-motion / subpixel case?
  - are the contaminated leading shells carrying high specular TA confidence even when the reprojection footprint is not fully valid?

### Trials

1. Added a new TA diagnostic pass:
   - `TASpecMotionInputs`
   - outputs `spec_accum`, `spec_history_quality`, and `allSamplesValid`
   - wired through:
     - `libraries/include/renderer/RenderConfig.h`
     - `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`
     - `libraries/source/renderer/renderer/GPURenderer.cpp`
     - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
2. Added a new investigation harness:
   - `tests/reblur/test_repeated_motion_ta_motion_regime.py`
   - reuses the same contaminated leading-shell extraction as the repeated-motion regressions
   - captures:
     - evaluated output (`TemporalAccumSpecular` or `Full`)
     - `TAMotionVector`
     - `TASpecMotionInputs`
     - `TADisocclusion`
     - `TAMaterialId`
3. Fixed one diagnostic mistake in the first draft:
   - the `TAMotionVector` blue channel quantized small magnitudes too aggressively
   - switched motion decoding to the signed `RG` channels instead
4. Verified and ran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_motion_regime.py`
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ta_motion_regime.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_motion_regime.py --framework macos --skip_build --eval_debug_pass Full`

### Findings

- The remaining repro in this test case is **not** a low-motion / subpixel regime.
  - On both `TemporalAccumSpecular` and `Full`, the contaminated leading shells report:
    - `motion_med ~= 33.55 px`
    - `motion_p90 ~= 33.55 px`
    - `sub1 = 0.00`
    - `sub0.5 = 0.00`
  - So the current repeated-motion failure is happening under clearly large camera motion, not under a hidden `< 1 px` regime.
- The TA confidence state on those contaminated shells is still over-trusting history:
  - `TemporalAccumSpecular`, worst-top3 contaminated shells:
    - `spec_accum ~= 2.18`
    - `spec_history_quality = 1.000`
    - `full_valid ~= 0.84` on the leading side
    - `full_valid ~= 0.90` on the trailing side
    - `partial&fullQ ~= 0.16`
  - `Full`, worst-top3 contaminated shells:
    - `spec_accum ~= 1.92`
    - `spec_history_quality = 1.000`
    - `full_valid ~= 0.85` on the leading side
    - `full_valid ~= 0.87` on the trailing side
    - `partial&fullQ ~= 0.15`
- The most useful direct evidence is the `partial&fullQ` metric:
  - about `8% - 20%` of contaminated leading-shell pixels are **not** full-footprint-valid
  - yet they still carry near-max `spec_history_quality`
- So the failing specular TA state is now more concrete:
  - history length has already been capped down to about `2` frames
  - but the quality term still stays pinned at `1.0`
  - that leaves the TA blend effectively governed by the capped EMA weight alone, even on partially invalid leading footprints

### Current conclusion

- The low-motion hypothesis for this repro is false.
- The stronger evidence now points at a more specific TA root cause:
  - contaminated leading shells are still in a high-trust specular TA regime
  - the trust signal does not drop even when the full reprojection footprint is not valid
  - that mismatch survives all the way to the visible `Full` output
- No denoiser fix was attempted in this task.
- The next useful investigation should compare **why** `spec_history_quality` stays pinned at `1.0` on those partially invalid leading shells, rather than trying another heuristic blend cap.

## Task 26: Correct the numeric screenshot decoding and re-check the TA motion / quality evidence before changing any shader logic

- **Status:** DONE
- **Scope:** The previous task inferred motion and quality directly from PNG screenshots. That needed verification because the screenshots come from an `SRGB` render target even when ACES is bypassed. This task corrected the diagnostic methodology before drawing any more causal conclusions.

### Trials

1. Verified the screenshot path:
   - `libraries/source/renderer/pass/ToneMappingPass.cpp`
   - `shaders/screen/tone_mapping.ps.slang`
   - confirmed:
     - ACES is bypassed for REBLUR debug passes
     - but the debug screenshots still go through an `SRGB` tone-mapping target
2. Re-read archived debug captures and found the contradiction:
   - the motion-regime script was treating `SRGB` bytes as linear values
   - that made zero / neutral debug pixels decode as very large signed motion
3. Strengthened the existing diagnostics:
   - updated `tests/reblur/test_repeated_motion_ta_motion_regime.py`
   - updated `tests/reblur/test_repeated_motion_ta_quality_delta.py`
   - both now decode continuous-value debug screenshots from `SRGB` to linear before measuring TA signals
4. Added one new TA probe to test the remaining “surface regime” hypothesis:
   - `TASpecSurfaceInputs`
   - outputs:
     - roughness
     - normalized specular hit distance
     - `GetSpecMagicCurve(roughness)`
   - wired through:
     - `libraries/include/renderer/RenderConfig.h`
     - `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`
     - `libraries/source/renderer/renderer/GPURenderer.cpp`
     - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
   - new harness:
     - `tests/reblur/test_repeated_motion_ta_spec_surface_regime.py`
5. Verified and ran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_motion_regime.py tests/reblur/test_repeated_motion_ta_quality_delta.py tests/reblur/test_repeated_motion_ta_spec_surface_regime.py`
   - `python3 build.py --framework macos`
   - `python3 tests/reblur/test_repeated_motion_ta_motion_regime.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_quality_delta.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_spec_surface_regime.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_motion_regime.py --framework macos --skip_build --eval_debug_pass Full`

### Findings

- The previous “large-motion” conclusion was wrong.
  - After correct `SRGB -> linear` decoding, the contaminated leading shells are in a **subpixel** camera-motion regime:
    - `TemporalAccumSpecular`, worst-top3:
      - `motion_med ~= 0.41 px`
      - `motion_p90 ~= 0.41 px`
      - `sub1 = 1.00`
      - `sub0.5 = 1.00`
    - `Full`, worst-top3:
      - `motion_med ~= 0.41 px`
      - `motion_p90 ~= 0.41 px`
      - `sub1 = 1.00`
      - `sub0.5 = 1.00`
- The pinned TA trust state survived that correction.
  - On the same contaminated shells:
    - `spec_history_quality = 1.000`
    - leading/trailing quality remains effectively `1.000 / 1.000`
    - `partial&fullQ ~= 0.14 - 0.16`
  - So the visible failure still happens while some leading-shell pixels are not full-footprint-valid, yet TA keeps them at full-quality trust.
- The corrected quality-delta probe says the missing surface footprint itself is effectively zero:
  - `1 - footprintQuality ~= 0`
  - `1 - spec_history_quality ~= 0`
  - even on the contaminated leading shells
  - so this is **not** a case where a large partial-footprint penalty is being ignored later
- The local TA code explains why the trust remains pinned in this corrected regime:
  - all additional specular history-confidence shaping is behind:
    - `if (motion_pixels > REBLUR_TA_MOTION_RESET_THRESHOLD_PX)`
  - and `REBLUR_TA_MOTION_RESET_THRESHOLD_PX = 1.0`
  - the contaminated shells are at about `0.41 px`, so that branch never runs
  - the result is that specular TA trust stays equal to near-perfect surface `footprintQuality`
- The extra surface probe did **not** support a narrower “only very smooth mirror-like surfaces” explanation:
  - contaminated shells included both rough and less-rough surfaces
  - so the strongest evidence is the **subpixel TA gate mismatch**, not a purely low-roughness classification
- This corrected interpretation also lines up better with the NRD reference:
  - NRD’s specular TA trust is not driven by a single hard `> 1 px` gate
  - it uses continuous surface / parallax confidence and a separate virtual-motion path
  - our current implementation has neither for this subpixel regime, so near-perfect surface reprojection can still over-trust stale specular history

### Current conclusion

- Task 25’s “33.55 px large-motion regime” finding was a diagnostic artifact caused by reading `SRGB` screenshots as linear values.
- The stronger corrected evidence is:
  - the remaining repeated-motion ghosting lives in a **subpixel** camera-motion regime (`~0.41 px`)
  - the TA specular confidence shaping never engages because of the hard `> 1 px` gate
  - on those same shells, surface-footprint trust is still effectively perfect, so TA keeps reusing specular history at full quality
- That is the clearest root-cause evidence so far, and it points specifically at:
  - the hard subpixel motion gate in local specular TA confidence logic
  - combined with the lack of an NRD-style continuous / virtual-motion specular confidence path
- No denoiser fix was attempted in this task.

## Task 27: Turn the corrected subpixel-gate finding into a dedicated failing regression

- **Status:** DONE
- **Scope:** The corrected evidence in Task 26 was strong, but it still lived across separate diagnostic scripts. This task turned that evidence into one dedicated regression that fails only when the root-cause signature is present:
  - contaminated leading shells exist
  - those shells stay below the local `1 px` TA motion gate
  - TA still reports pinned full-quality trust on them

### Trials

1. Added a new regression:
   - `tests/reblur/test_repeated_motion_ta_subpixel_gate.py`
   - captures:
     - evaluated output (`TemporalAccumSpecular` or `Full`)
     - `TAMotionVector`
     - `TASpecMotionInputs`
     - `TADisocclusion`
     - `TAMaterialId`
2. The new regression reuses the same motion-leading shell extraction as the existing repeated-motion tests, but fails on a narrower signature:
   - `lead_ratio >= 1.15`
   - `motion_med < 1.0 px`
   - `sub1 >= 0.95`
   - `spec_quality >= 0.99`
   - `partial&fullQ >= 0.05`
3. Verified and ran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_subpixel_gate.py`
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass Full`

### Findings

- The new regression fails cleanly at the TA stage:
  - `TemporalAccumSpecular`
  - worst-top3:
    - `lead_ratio = 1.74x`
    - `motion_med = 0.41 px`
    - `sub1 = 1.00`
    - `quality = 1.000`
    - `partial&fullQ = 0.14`
- The same root-cause signature survives to visible output:
  - `Full`
  - worst-top3:
    - `lead_ratio = 1.22x`
    - `motion_med = 0.41 px`
    - `sub1 = 1.00`
    - `quality = 1.000`
    - `partial&fullQ = 0.19`
- So the corrected root-cause evidence is now encoded as a single reproducible failing testcase, not just a narrative interpretation across multiple diagnostics.

### Current conclusion

- The investigation no longer needs more localization before a fix attempt.
- The remaining bug is now pinned down to a specific failure mode:
  - contaminated specular TA reuse under repeated camera motion
  - in a subpixel regime that never crosses the local `> 1 px` confidence gate
  - while TA quality remains pinned at full trust
- That conclusion is consistent with the upstream NRD reference:
  - NRD does not rely on this single hard subpixel gate for specular trust
  - it uses continuous surface / parallax confidence plus virtual-motion handling
- No denoiser fix was attempted in this task.

## Task 28: Probe the TA gate boundary directly and fix the remaining motion-diagnostic precision problem

- **Status:** DONE
- **Scope:** After Task 27, the remaining question was whether the repeated-motion root-cause signature actually tracks the local TA motion gate in a measurable way. This task did two things:
  - parameterized the repeated camera-nudge testcase so the yaw step can be swept from the command line
  - fixed the motion diagnostic precision so subpixel shell motion can be measured more accurately

### Trials

1. Added a test-only repeated-motion yaw-step override:
   - `reblur_ghosting_yaw_step`
   - wired through:
     - `libraries/include/renderer/RenderConfig.h`
     - `libraries/source/renderer/RenderConfig.cpp`
     - `tests/reblur/ReblurGhostingTest.cpp`
   - `ReblurGhostingTest` now uses `app.GetRenderConfig().reblur_ghosting_yaw_step` instead of a hardcoded `3.0` degree step.
2. Tried one TA shader fix based on the new root-cause regression:
   - continuous subpixel specular confidence
   - NRD-style confidence scaling of spec accumulation speed
3. Rebuilt and reran the new root-cause regression:
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass Full`
4. Result of that shader trial:
   - it regressed both the TA-stage and visible subpixel-gate regression
   - `TASpecMotionInputs` still reported `quality = 1.000` on the failing shells
   - reverted the shader trial
5. Parameterized the motion diagnostic itself:
   - added `TAMotionVectorFine`
   - wired through:
     - `libraries/include/renderer/RenderConfig.h`
     - `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`
     - `libraries/source/renderer/renderer/GPURenderer.cpp`
     - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
   - updated:
     - `tests/reblur/test_repeated_motion_ta_subpixel_gate.py`
     - `tests/reblur/test_repeated_motion_ta_motion_regime.py`
   - the new fine pass uses a tighter motion scale to preserve useful precision below `1 px`
6. Fixed one follow-up decoding bug:
   - the scripts initially still used the old coarse motion scale
   - updated the motion decode constant from `100.0` to `10.0`
7. Rebuilt and reran:
   - `python3 -m py_compile tests/reblur/test_repeated_motion_ta_subpixel_gate.py tests/reblur/test_repeated_motion_ta_motion_regime.py`
   - `python3 build.py --framework macos`
   - baseline:
     - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass Full`
   - larger nudge:
     - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass Full --reblur_ghosting_yaw_step 6.0`
   - also attempted:
     - `--reblur_ghosting_yaw_step 9.0`
     - but that moved too far for the current component matcher and produced too few matched components

### Findings

- The reverted TA shader trial was a dead end:
  - it made the new root-cause regression worse
  - and still failed to move `spec_history_quality` off `1.000` on the contaminated shells
- The fine motion diagnostic corrected the shell-motion estimate again:
  - the contaminated shells are not just subpixel; in the current repeated-motion testcase they are around:
    - `motion_med ~= 0.04 px`
  - with:
    - `sub1 = 1.00`
    - `quality = 1.000`
    - `partial&fullQ ~= 0.15 - 0.20`
- Increasing the global yaw step from `3°` to `6°` did **not** move the failing shell motion out of that regime:
  - baseline `Full`, worst-top3:
    - `lead_ratio = 1.25x`
    - `motion_med = 0.04 px`
    - `quality = 1.000`
  - `6°` sweep, worst-top3:
    - `lead_ratio = 1.19x`
    - `motion_med = 0.04 px`
    - `quality = 1.000`
- So the local failure is even more specific than before:
  - the visible contaminated shells live in an extremely low local motion regime
  - and changing the global camera nudge within the currently analyzable range does not push those shells near the `1 px` TA gate
- The `9°` sweep was too large for the current shell-matching logic, so it did not produce a useful threshold-crossing measurement.

### Current conclusion

- The root-cause signature from Task 27 is still valid, and the fine motion pass strengthens it:
  - repeated-motion ghosting is happening on shells with about `0.04 px` local motion
  - those shells keep fully pinned TA trust
  - and they remain in that regime even when the global nudge size is increased to `6°`
- The reverted shader trial says the next fix should not be another broad continuous-confidence tweak without first explaining why the current TA quality debug signal remains pinned on those exact shells.
- No denoiser fix was retained in this task.

## Task 29: Replace the dead subpixel gate with a continuous TA specular confidence path and verify the visible regression

- **Status:** DONE
- **Scope:** The investigation had already isolated the bug to specular TA reuse under repeated camera motion. This task switched from diagnosis to implementation:
  - replace the `> 1 px`-gated specular trust reduction with a continuous confidence term
  - keep the fix TA-local and guided by the repeated-motion regressions
  - verify against the original visible contamination gate, not just the new root-cause diagnostic

### Trials

1. Re-read the local TA shader against upstream NRD `REBLUR_TemporalAccumulation.cs.hlsl`.
   - Confirmed the local code still gated its only continuous specular-confidence path behind:
     - `motion_pixels > REBLUR_TA_MOTION_RESET_THRESHOLD_PX`
   - Confirmed the local specular payload already carries enough signal for a contained fix:
     - `current_spec.a` / `hist_spec.a` = normalized hit distance
     - `nr.z` = roughness
     - `prev_spec_accum_speed` = history length proxy
2. Implemented a new TA-side helper in:
   - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
   - supporting constants in:
     - `shaders/include/reblur_config.h.slang`
3. The new retained logic:
   - computes a continuous specular surface-history confidence from:
     - surface motion in pixels
     - normalized specular hit distance
     - roughness via `GetSpecMagicCurve`
     - previous specular accumulation speed
   - interprets those as a simple virtual-motion proxy:
     - sharp, far-hit reflections tolerate much less motion than rough, near-hit reflections
   - applies that confidence below the old `1 px` gate instead of waiting for a hard threshold crossing
   - keeps the old larger-motion surface gate as an additional cap when motion actually exceeds `1 px`
4. Rebuilt:
   - `python3 build.py --framework macos`
5. Reran the new root-cause regression at the TA stage:
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass TemporalAccumSpecular`
6. Reran the new root-cause regression on visible output:
   - `python3 tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework macos --skip_build --eval_debug_pass Full`
7. Reran the original user-facing contamination regression:
   - `python3 tests/reblur/test_repeated_motion_contamination.py --framework macos --skip_build --eval_debug_pass Full`
8. Reran the broader motion stability regression:
   - `python3 tests/reblur/test_denoised_motion_luma.py --framework macos --skip_build`
9. Tried one follow-up retune:
   - increased `REBLUR_TA_SPEC_VIRTUAL_MOTION_SCALE` from `16.0` to `20.0`
10. Rebuilt and reran the root-cause gate on `Full`.
11. That retune did not help, so it was reverted and the original `16.0` checkpoint was rebuilt and retained.
12. Re-ran the user-facing contamination regression and motion-luminance regression on the retained checkpoint after the revert.

### Findings

- The retained TA confidence change materially improved the bug:
  - `TemporalAccumSpecular` root-cause gate improved from about:
    - `lead_ratio = 1.74x`
    - to:
    - `lead_ratio = 1.31x`
  - visible `Full` root-cause gate improved from about:
    - `lead_ratio = 1.25x`
    - to:
    - `lead_ratio = 1.17x`
- The root-cause gate is still red on the retained checkpoint because its `TASpecMotionInputs` capture still quantizes the confidence channel high enough to report:
  - `quality = 1.000`
  - even though the visible artifact is substantially reduced
- The user-facing regression that originally reproduced the bug is now green on the retained checkpoint:
  - `test_repeated_motion_contamination.py --eval_debug_pass Full`
  - results:
    - `Nudge 0: 1.07x`
    - `Nudge 1: 1.10x`
    - `Nudge 2: 1.11x`
    - `Nudge 3: 1.16x`
    - `Nudge 4: 1.15x`
    - worst leading fast/settled ratio: `1.16x`
    - worst single-component leading ratio: `1.20x`
    - worst lead/trail asymmetry: `1.03x`
  - so it now passes the visible repeated-motion gate with margin against:
    - `1.20x` top-3
    - `1.25x` worst single-component
- The broader denoised-only motion stability regression still passes on the retained checkpoint:
  - `test_denoised_motion_luma.py`
  - `5 passed, 0 failed`
- The `20.0` virtual-motion retune was a dead end:
  - it did not improve the root-cause diagnostic
  - it was reverted

### Current conclusion

- A retained fix now exists for the reported issue:
  - repeated-motion visible ghosting on history-valid leading shells is reduced enough for the original regression to pass
  - the change is localized to specular temporal accumulation
  - it follows the NRD direction of continuous specular trust rather than a hard surface-motion gate
- The remaining red root-cause diagnostic is now narrower than the original bug:
  - it says the current `TASpecMotionInputs` debug capture still reads the spec-history-quality channel as saturated on the failing shells
  - not that the visible repeated-motion contamination regression is still failing
- The retained checkpoint is:
  - continuous TA specular virtual-motion confidence in `reblur_temporal_accumulation.cs.slang`
  - constants in `reblur_config.h.slang`
  - visible contamination regression: PASS
  - motion-luminance regression: PASS
