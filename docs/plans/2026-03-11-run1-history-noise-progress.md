# Run 1 History Noise Investigation Progress

## Date: 2026-03-11

## Task 1: Audit current REBLUR background, regressions, and harnesses

- **Status:** DONE
- **Scope:** Re-read the current REBLUR design note, the most recent motion / floor / regression progress logs, and the active camera-nudge / motion-shell Python and C++ tests before trusting any prior conclusion.

### Trials

1. Read project-level testing and contribution guidance:
   - `README.md`
   - `docs/CONTRIBUTING.md`
   - `docs/Test.md`
2. Read the REBLUR design / investigation history most relevant to the reported symptom:
   - `docs/plans/2026-02-25-reblur-design.md`
   - `docs/plans/2026-02-27-reblur-camera-motion-progress.md`
   - `docs/plans/2026-03-05-floor-noise-progress.md`
   - `docs/plans/2026-03-05-reblur-regression-progress.md`
   - `docs/plans/2026-03-09-reblur-motion-ghosting-progress.md`
3. Read the currently active test harnesses and suite wiring:
   - `tests/reblur/ReblurConvergedHistoryTest.cpp`
   - `tests/reblur/test_converged_history.py`
   - `tests/reblur/test_motion_side_history.py`
   - `tests/reblur/test_repeated_motion_stage_settled_delta.py`
   - `tests/reblur/reblur_test_suite.py`
4. Audited current shader / C++ entry points related to the reported issue:
   - `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
   - `shaders/ray_trace/reblur_final_history.cs.slang`
   - `shaders/ray_trace/reblur_stabilize_albedo.cs.slang`
   - `shaders/include/reblur_reprojection.h.slang`
   - `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`
   - `libraries/include/renderer/RenderConfig.h`

### Findings

- The current tree already contains several overlapping regressions for camera-nudge noise, but the later progress notes explicitly show that previous root-cause claims were revised multiple times. The latest notes cannot be treated as ground truth without fresh reproduction on the current source tree.
- `test_converged_history.py` still evaluates the exact user-facing Run 1 end-to-end path, but its main semantic segmentation is dominated by `TADisocclusion` and a floor-only sub-check. That means it can still miss per-object history-valid shells if those shells are noisy while the floor remains stable.
- The repo already has stronger object-shell diagnostics:
   - `test_motion_side_history.py`
   - `test_run1_semantic_e2e.py`
   - repeated-motion contamination / TA-confidence scripts
  These should be preferred for localization before introducing a new bespoke harness.
- The active C++ nudge testcase still uses a fixed `2.0` degree yaw and only waits `5` settle frames after the nudge. That is intentional: it is a regression for history reuse under a small nudge, not a full reconvergence test.
- Current shader structure suggests there are at least four places where a “history-valid but still noisy” object shell can be introduced:
   - partial-footprint reprojection in temporal accumulation
   - confidence / accumulation-speed shaping in temporal accumulation
   - spatial re-filtering after temporal accumulation
   - displayed-color reuse in `reblur_final_history.cs.slang`
- Next step: reproduce the current Run 1 output on the active platform/build, then compare it against the object-shell regressions to see whether the bug is already covered and currently red.

## Task 2: Reproduce the reported Run 1 failure on the active Windows / GLFW tree

- **Status:** DONE
- **Command:** `python tests/reblur/test_converged_history.py --framework glfw --skip_build`
- **Reason:** Verify the exact user-reported end-to-end Run 1 behavior on the current local source tree before trusting the macOS-heavy earlier progress notes.

### Findings

- The current GLFW result reproduces a real regression immediately: the test fails with **11 passed, 3 failed**.
- Run 1 quality is still visibly wrong even though the semantic mask says nearly all pixels are history-valid:
  - **End-to-end FLIP vs vanilla:** `0.0765` (passes the loose whole-frame metric)
  - **History-valid reprojection:** `653681 / 653709` geometry pixels = **100.0%**
  - **Mean footprint quality:** **1.000**
  - **Disoccluded pixels:** only **28** pixels
- Despite that near-perfect history-valid classification, the denoised result remains much noisier than the vanilla reference:
  - **History HF residual ratio:** `2.38x` (`0.038151 / 0.016008`) -> **FAIL**
  - **Run 1 floor local_std after / vanilla:** `1.203x` -> **FAIL**
  - **Run 1 floor local_std after / before:** `1.115x` -> **FAIL**
- Luminance is mostly preserved, so this is not primarily an energy-loss bug:
  - **Run 1 mean luma / vanilla:** `0.9736`
  - **Floor luma after / vanilla:** `0.988x`
  - **Floor luma after / before:** `0.987x`
- Conclusion:
  - The current tree is definitely in the state the user is complaining about.
  - The failure is stronger than “object shells are noisy”; even the floor regresses again on GLFW.
  - The semantic mask is currently overconfident: it reports essentially universal valid history while the output quality contradicts that.

### Next step

- Run the existing object-shell regressions (`test_motion_side_history.py`, `test_run1_semantic_e2e.py`, and stage-delta diagnostics) on the same GLFW tree to determine whether the visible per-object noise is already localized by existing coverage or whether a new test is actually needed.

## Task 3: Re-run the existing object-shell regressions on the current GLFW tree

- **Status:** DONE
- **Commands:**
  - `python tests/reblur/test_motion_side_history.py --framework glfw`
  - `python tests/reblur/test_run1_semantic_e2e.py --framework glfw`
  - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build`
  - `python tests/reblur/test_repeated_motion_ta_confidence.py --framework glfw --skip_build --ta_channel specular`
  - `python tests/reblur/test_repeated_motion_ta_subpixel_gate.py --framework glfw --skip_build --eval_debug_pass TemporalAccumSpecular`

### Findings

- The existing coverage already catches the user-visible problem on GLFW. A new reproduction test was **not** needed just to prove the symptom.
- `test_motion_side_history.py` fails on both full-pipeline and denoised-only outputs:
  - **Full pipeline top-3 leading HF ratio mean:** `3.16x`
  - **Denoised-only top-3 leading HF ratio mean:** `3.10x`
  - **Median leading valid fraction:** `1.000`
- `test_run1_semantic_e2e.py` fails the exact user-visible Run 1 complaint:
  - **2 components** exceed the allowed visible wrong-side shell arcs (limit `1`)
  - Failing components on this GLFW run: `65`, `70`
- Repeated-motion coverage is also red on GLFW:
  - `test_repeated_motion_contamination.py` (Full) fails at **`1.62x`** top-3 leading fast/settled ratio
  - So this is not just a single-nudge artifact
- The repeated-motion TA-confidence test did **not** show excessive specular accumulation length:
  - contaminated leading shells still report **top-3 spec accum ~`2.09` frames**
  - `spec_history_quality` debug still reads **`1.000`**
- The stronger root-cause regression is also red:
  - `test_repeated_motion_ta_subpixel_gate.py` reports contaminated shells with:
    - **lead_ratio:** `1.65x`
    - **motion_med:** `0.04 px`
    - **sub1:** `1.00`
    - **quality:** `1.000`
    - **partial&fullQ:** `0.11`

### Conclusion after Task 3

- The current failure mode on GLFW is:
  - history-valid motion-leading shells remain visibly wrong
  - they are in an extremely low-motion regime
  - TA debug says quality is still effectively full-trust on them
  - but TA accumulation length is only about 2 frames on the contaminated shells
- That means the current tree is internally inconsistent:
  - the shell is treated as valid enough to keep quality pinned,
  - but not valid enough to retain enough specular history to stay clean.

### Next step

- Re-localize whether the leading-shell damage is already present in raw specular history or whether temporal accumulation is making it worse.

## Task 4: Bound all repeated-motion wrappers to the test case's actual frame budget

- **Status:** DONE
- **Reason:** A stage probe (`CompositeDiffuse`) took far too long because the repeated-motion Python wrappers relied on a 900 second subprocess timeout instead of a frame timeout, even though `ReblurGhostingTest.cpp` encodes a finite deterministic sequence.

### Trials

1. Read `tests/reblur/ReblurGhostingTest.cpp` and derived the expected sequence budget:
   - `StartupDelayFrames = 10`
   - `WarmupFrames = 30`
   - `NudgeCount = 5`
   - `FastCaptureFrames = 2`
   - `SettleFrames = 30`
   - expected total budget is about **200-210 frames**
2. Added `--test_timeout 260` to every Python wrapper that runs `reblur_ghosting`.
3. Updated the direct C++ ghosting entry in `tests/reblur/reblur_test_suite.py` from `120` to `260` frames.
4. Re-ran the previously problematic command:
   - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass CompositeDiffuse`

### Findings

- The previous long-running stage probe now fails in a bounded, reasonable time instead of burning wall-clock time:
  - `CompositeDiffuse` now returns in about **46s** with a real result:
    - **top-3 leading fast/settled ratio:** `1.31x`
    - **worst single-component:** `1.42x`
- The runtime issue was therefore a harness bug, not a justified test duration.

### Files Modified

- `tests/reblur/test_repeated_motion_contamination.py`
- `tests/reblur/test_repeated_motion_ta_confidence.py`
- `tests/reblur/test_repeated_motion_stage_fast_delta.py`
- `tests/reblur/test_repeated_motion_stage_settled_delta.py`
- `tests/reblur/test_repeated_motion_ta_quality_delta.py`
- `tests/reblur/test_repeated_motion_ta_spec_settled_asymmetry.py`
- `tests/reblur/test_repeated_motion_ta_motion_regime.py`
- `tests/reblur/test_repeated_motion_ta_subpixel_gate.py`
- `tests/reblur/test_repeated_motion_ta_spec_surface_regime.py`
- `tests/reblur/test_repeated_motion_ts_spec_antilag_inputs.py`
- `tests/reblur/test_repeated_motion_ts_spec_blend.py`
- `tests/reblur/test_repeated_motion_ts_spec_clamp_inputs.py`
- `tests/reblur/reblur_test_suite.py`

### Next step

- Use the bounded stage diagnostics to determine whether TA is already corrupting specular shell history, or whether the damage is coming later in the pipeline.

## Task 5: Re-localize the specular shell failure by stage

- **Status:** DONE
- **Commands:**
  - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass TemporalAccum`
  - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass CompositeDiffuse`
  - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass CompositeSpecular`
  - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass TemporalAccumSpecular`
  - `python tests/reblur/test_repeated_motion_stage_fast_delta.py --framework glfw --skip_build --base_debug_pass TASpecHistory --compare_debug_pass TemporalAccumSpecular`

### Findings

- The current GLFW regression is already strong in temporal accumulation:
  - `TemporalAccum`: **`1.76x`** top-3 leading fast/settled ratio
  - `TemporalAccumSpecular`: **`1.66x`**
- The visible output tracks the specular channel much more closely than the diffuse term:
  - `CompositeDiffuse`: **`1.31x`**
  - `CompositeSpecular`: **`1.60x`**
  - `Full`: **`1.62x`** (from Task 3)
- So the user-visible contamination is mainly a **specular TA** problem on this GLFW tree.
- The fast-stage delta against raw specular history is especially important:
  - `TASpecHistory -> TemporalAccumSpecular`: **`1.43x`** top-3 leading compare/base ratio
  - worst single-component amplification: **`2.00x`**
- Interpretation:
  - raw reprojected specular history is not perfect,
  - but **temporal accumulation itself makes those leading shells materially noisier** instead of preserving or improving them.

### Additional trial and rejection

- Trialed a broad TA-side relaxation that scaled specular motion history back up toward the full accumulation horizon.
- Result:
  - `test_converged_history.py` got worse on GLFW:
    - history noise ratio `2.38x -> 2.72x`
    - floor after/vanilla ratio `1.20x -> 1.40x`
- Rejected that broader relaxation and reverted to a narrower tuning direction.

### Conclusion after Task 5

- The current best localization is:
  - the problem is not primarily final-history reuse,
  - not primarily PT blend,
  - and not primarily diffuse-only.
- The shortest path forward is to fix **specular temporal accumulation blending / confidence on history-valid motion-leading shells** without broadly re-enabling stale specular reuse everywhere.

## Task 6: Probe specular TA reuse hypotheses and reject the ones that do not move the target regressions

- **Status:** DONE
- **Goal:** Test whether the noisy shell is primarily caused by:
  - an overly aggressive global specular motion cap,
  - center-fallback internal-data under-trust on partial footprints, or
  - a missing small-motion specular reuse floor.

### Trials

1. **Broad specular history relaxation**
   - Trial: scaled specular motion history back toward the full accumulation horizon.
   - Result: **rejected**
   - `test_converged_history.py` got worse on GLFW:
     - history noise ratio `2.38x -> 2.72x`
     - floor after/vanilla ratio `1.20x -> 1.40x`
   - Conclusion: broad re-enabling of specular history reintroduces stale wrong history faster than it removes noise.

2. **Raise specular motion cap from 3 to 8**
   - Trial: `REBLUR_TA_SPEC_MOTION_MAX_ACCUM = 8.0`
   - Result: **rejected**
   - Repeated-motion TA metrics stayed effectively unchanged:
     - contaminated top-3 spec accum remained about `2.09`
   - Conclusion: the simple upper-cap constant is not the active limiter in the reproduced repeated-motion shells.

3. **Preserve stronger internal-data history on center-fallback partial footprints**
   - Trial:
     - preserved dominant valid-tap accum speeds when center-sample color fallback was used
     - also tried taking the strongest valid specular accum tap inside the accepted footprint
   - Result: **rejected**
   - `TASpecHistory -> TemporalAccumSpecular` fast-stage amplification stayed unchanged:
     - top-3 compare/base ratio remained **`1.43x`**
   - Conclusion: the dominant failure is not limited to the partial-footprint center-fallback path.

4. **Global tiny-motion, high-quality specular reuse floor**
   - Trial: added a modest specular reuse floor for subpixel, apparently high-quality motion.
   - Result: **rejected**
   - `test_repeated_motion_ta_confidence.py` still reported the same contaminated-shell spec accum values:
     - top-3 spec accum remained about **`2.10`**
   - Conclusion: the plain RGB debug views likely hide a non-trivial spec-history-quality penalty, and this floor did not engage materially on the failing shells.

### Final state after Task 6

- All TA shader experiments from this task were reverted.
- The retained source changes from this turn are:
  - the repeated-motion test timeout fixes
  - the new progress note documenting the current GLFW findings

### Current conclusion

- The investigation has ruled out several easy-but-wrong explanations.
- The remaining evidence still says:
  - history-valid leading shells are being degraded in **specular temporal accumulation**
  - raw spec history is better than TA output on those shells
  - but the simple debug views are not precise enough to tell whether the decisive problem is:
    - a hidden spec-history-quality penalty,
    - an over-conservative spec-weight floor,
    - or some other TA confidence term that is not visible enough through the current screenshot-based diagnostics.

### Next useful step

- Add a more precise single-nudge TA shell diagnostic for `reblur_converged_history` that reads the exact shell-local specular TA inputs used in the user-reported Run 1 setup, instead of inferring them from repeated-motion screenshots alone.

## Task 7: Replace the loose ghosting timeout with a phase-derived frame budget

- **Status:** DONE
- **Reason:** The earlier `260`-frame timeout fixed the worst harness bug, but it was still a blunt guess. The ghosting sequence is deterministic, so the timeout should be derived from the test case itself and kept tight enough to expose real stalls.

### Trials

1. Re-ran the raw C++ ghosting test directly to measure the actual pass frame:
   - `python build.py --framework glfw --skip_build --run --test_case reblur_ghosting --headless true --clear_screenshots true --test_timeout 400`
2. Read the resulting log against `tests/reblur/ReblurGhostingTest.cpp`.
3. Centralized the repeated-motion timeout budget in a new helper:
   - `tests/reblur/ghosting_budget.py`
4. Replaced all duplicated `260` literals in:
   - `tests/reblur/reblur_test_suite.py`
   - all repeated-motion Python diagnostics that run `reblur_ghosting`
5. Documented the expected pass frame directly in `tests/reblur/ReblurGhostingTest.cpp`.
6. Re-ran the previously slow representative stage probe:
   - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass CompositeDiffuse`

### Findings

- The raw GLFW ghosting run passes at **frame 213** on the current tree:
  - baseline screenshot captured at frame **43**
  - final settled screenshot captured at frame **213**
- A tighter shared timeout of **230 frames** is enough:
  - expected pass frame: **213**
  - explicit slack: **17** frames
- The representative repeated-motion stage probe still reaches a real analysis result with the tighter cap:
  - command now runs with `--test_timeout 230`
  - it still fails for the actual regression, not for timeout:
    - top-3 leading fast/settled ratio **`1.31x`**
    - worst single-component ratio **`1.42x`**
- Conclusion:
  - the correct fix here was not “raise the timeout until it works”
  - the deterministic ghosting family now has a bounded frame budget derived from the test sequence itself
  - if these runs start exceeding about **230 frames**, that is now treated as a real harness or renderer problem

## Task 8: Tighten the Run 1 FLIP gate so the visibly bad e2e state no longer passes it

- **Status:** DONE
- **Reason:** The user correctly pointed out that the whole-frame FLIP sub-check inside `test_converged_history.py` was still passing even when Run 1 looked obviously wrong after the nudge.

### Trials

1. Re-ran the current GLFW test:
   - `python tests/reblur/test_converged_history.py --framework glfw --skip_build`
2. Re-read the earlier threshold-tightening notes in:
   - `docs/plans/2026-03-05-floor-noise-progress.md`
   - `docs/plans/2026-03-05-reblur-regression-progress.md`
3. Tightened `E2E_FLIP_MAX` in:
   - `tests/reblur/test_converged_history.py`
4. Re-ran the full GLFW semantic test after the threshold change:
   - `python tests/reblur/test_converged_history.py --framework glfw --skip_build`

### Findings

- The reproduced bad GLFW Run 1 state still passes the old FLIP gate:
  - current re-run: **`E2E FLIP vs vanilla = 0.0810`**
  - earlier re-run in this investigation: **`0.0765`**
  - old threshold: **`0.14`**
- That means the old FLIP threshold was not just loose; it was too loose to reject the exact regression the user is looking at.
- Tightened `E2E_FLIP_MAX` from **`0.14`** to **`0.07`**.
- Rationale:
  - whole-frame mean FLIP is still only a secondary sanity check here, because localized motion-leading shell noise gets diluted by the rest of the frame
  - but it should at least fail on the known-bad Run 1 states now reproducing around **`0.0765` - `0.0810`**
- Verification after the change:
  - GLFW re-run now reports **`FAIL: FLIP 0.0810 > 0.07`**
  - overall test result changed from **3 failures** to **4 failures**, because the FLIP sub-check is now red in addition to the existing floor/history failures

## Task 9: Treat timeout as a bug-only guard, not as a normal way for ghosting runs to finish

- **Status:** DONE
- **Reason:** The user is correct that the repeated-motion harness should never rely on timeout to “finish collecting results”. `reblur_ghosting` already has an explicit success exit once the required screenshots are captured; timeouts should only catch hangs or broken state machines.

### Trials

1. Re-read the active C++ screenshot tests:
   - `tests/reblur/ReblurGhostingTest.cpp`
   - `tests/screenshot/MultiFrameScreenshotTest.cpp`
2. Confirmed both tests already return `Pass` explicitly once all required screenshots are captured.
3. Added a shared ghosting harness helper:
   - `tests/reblur/ghosting_harness.py`
4. Updated all repeated-motion Python wrappers to use that helper instead of their duplicated local `run_app(...)` implementations.
5. Re-ran a representative repeated-motion test:
   - `python tests/reblur/test_repeated_motion_contamination.py --framework glfw --skip_build --eval_debug_pass CompositeDiffuse`

### Findings

- The real bug was not that `reblur_ghosting` lacked an explicit exit; it already exits when all 11 screenshots are captured.
- The harness bug was that the repeated-motion wrappers treated the app like a generic long-running subprocess with a loose `timeout=900` wall-clock guard and no explicit “timeout means broken” handling.
- The new shared helper now treats both failure modes as bugs:
  - hitting the **230-frame** test guard
  - exceeding the **180s** wall-clock guard
- Repeated-motion wrappers no longer each carry their own copy of the old `subprocess.run(... timeout=900)` helper.
- Verification run still reached a real analysis result and failed for the actual regression:
  - `CompositeDiffuse` contamination remains **`1.31x`**
  - worst single-component remains **`1.43x`**

### Conclusion

- For these ghosting/repeated-motion cases:
  - **explicit pass** is the only normal completion path
  - **timeout** is a failure guard that indicates a harness/app bug and should be fixed before trusting further investigation
