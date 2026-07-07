# NRD review fixes

Findings from the review of commit `4aa61bd` ([NRD] nrd implementation), ordered by priority.
Each item lists the anchor location and the acceptance check.

Status 2026-07-07: P0 + P1 fixed and verified; P2, P4, P5 done (two items closed as
reviewed/no-change with measurements); P3 has two open architecture items (NrdConfig ownership,
denoiser state machine) plus the CI fold-in from P4.

## P0 — CI integrity

* [x] Vacuous NRD CI gate: on no-ray-tracing runners the probe silently falls back gpu→forward
  (`libraries/source/renderer/RenderConfig.cpp:84`), so the skip detection in
  `tests/nrd/run_nrd_gates.py:53` never fires and all gates pass against forward-rendered frames.
  Fixed: `RenderConfig::Validate` logs `effective pipeline: <name>`; the probe requires the
  `effective pipeline: gpu` marker as positive proof and fails loudly when it is missing
  (SKIPPED only with --allow_unsupported + the fallback warning).
  Verified: marker present on gpu runs, `effective pipeline: forward` on forward runs.
* [x] iOS build break: spirv-cross include/libs are macOS-branch-only (`CMakeLists.txt:358-367`)
  but iOS compiled `MetalNrdBackend.mm` (FRAMEWORK_APPLE guard) and `tests/nrd/NrdProbeTest.mm`
  (no guard). Fixed: backend guards narrowed to FRAMEWORK_MACOS (header, .mm, MetalRHI override);
  probe guarded to macOS; iOS falls back to the base `CreateNrdBackend` (nullptr).
  Verified: both files compile to empty TUs against the iphoneos SDK; full local iOS build blocked
  only by code signing (CI has certs).

## P1 — Denoiser functional bugs

* [x] Projection Y-flip: the Vulkan-style flipped projection (`CameraRenderProxy.cpp:93`) was
  passed unmodified as NRD `viewToClipMatrix`; NRD decomposes with D3D conventions, so
  matrix-derived reprojection was vertically mirrored vs IN_MV, breaking pitch/roll/vertical
  translation. Fixed: negate row 1 before the handoff (`NrdDenoiser.cpp`, RenderReblur).
  Verified: new pitch sweep arm — healthy 0.0086 vs 0.0140 with the mirror reintroduced
  (specular ghosting + raw-noise breakthrough confirmed visually); gate calibrated to 0.012.
* [x] Sky-pixel NaN normals: miss pixels have normal (0,0,0) and `nrd_pack.cs.slang` wrote
  `normalize(0)` = NaN into IN_NORMAL_ROUGHNESS; ReBLUR temporal accumulation averages neighbor
  normals unguarded → NaN at sky silhouettes. Fixed: is_hit guard writes (0,0,1) for sky.
  Verified: full gate suite green.
* [x] Uninitialized NrdOutputHistory: the resolve read the never-cleared private texture
  unconditionally and `lerp(a, NaN, 0)` propagates NaN permanently. Fixed: the history read is
  skipped when stabilizationBeta == 0; the write re-initializes the texture.
* [x] Handoff never exactly converges: `HandoffEndSamples` was hardcoded 2048 and the EMA was
  unconditional, so the frozen frame was never exactly the accumulator. Fixed: the handoff weight
  is computed from the post-dispatch sample count and pinned to 1 (beta 0) on the final resolve
  before the freeze; the shader selects the accumulator outright at weight 1 (NaN-proof).
  max_spp below the handoff window opts out so the motion harnesses (max_spp 1) keep filming
  ReBLUR output. Verified: nrd-on frozen screenshot is BIT-IDENTICAL to the nrd-off accumulator
  at max_spp 512.
* [x] Init-failure retry storm: failed `EnsureEnabledResources()` was never latched, re-running
  backend creation + pipeline compilation every frame and leaking one `nrd::Instance` per frame.
  Fixed: failures latch permanently (`enabled_resources_failed_`, surfaced via `IsActive()`).
* [x] Enable-after-convergence garbage: enabling from the control panel after the render freeze
  pointed tone mapping at a never-written texture. Fixed: the toggle branch restarts accumulation
  (MarkPixelDirty) when the accumulator is already frozen.
* [x] Seed determinism: `seed_counter_` was never reset, so converged output was not
  bit-reproducible run-to-run. Fixed: the seed resets (with an accumulator restart) when the scene
  finishes loading; it still never resets on ordinary accumulator clears (the seed-pinning trap).
  The write-only `dispatched_sample_count_` is removed. Verified: two headless runs bit-identical
  (nrd on and off).
* [x] Unclamped Int config entry: DragInt now passes `ImGuiSliderFlags_AlwaysClamp`
  (CTRL+click keyboard entry ignored the drag bounds and wrapped through uint32_t).
* [x] Enable-state sampled twice per frame: `Update()` and `Render()` read the cross-thread config
  independently. Fixed: sampled once per frame into `nrd_frame_active_` (GPURenderer::Update) and
  used for write_gbuffer, the denoiser passes, and the toggle detection.

### Found while fixing

* [x] Motion sweep gates can silently film frozen/stale frames and pass vacuously (this bit an
  intermediate version of these fixes: 15 of 16 captures were black and the gate still passed).
  Fixed: nrd_motion_test asserts all captures are pairwise distinct; verified it flags the broken
  captures and stays quiet on healthy ones.

## P2 — Efficiency

* [x] ~200 MB VRAM at 1080p with NRD off: six full-res RGBA32F G-buffer targets were allocated
  unconditionally. Fixed: 1x1 UAV dummies while disabled; real targets allocated in
  `EnsureEnabledResources`; `GPURenderer::BindNrdGBuffer` re-binds on first enable.
* [x] Compact NRD texture formats: R32F viewZ; RGBA16F for MV, normals, radiance, validation, and
  the motion/spec-albedo G-buffer targets. Kept RGBA32F: radiance G-buffers (HDR + hitT
  precision), normal+viewZ (disocclusion plane precision), albedo+objID (objID exactness), and
  output/history (the final resolve copies the accumulator through bit-exact).
* [x] Gate `write_gbuffer` off once handoff completes (`NrdDenoiser::NeedsGBuffer`); the resolve
  provably ignores the G-buffer at weight 1, and debug views keep it live.
* [x] Startup pipeline compilation: NOT parallelized — measured 54 ms for all 14 ReBLUR pipelines
  (nrd_probe log timestamps), so the "seconds of startup" premise was wrong.
* [x] Per-frame vector-of-vectors in `RenderReblur` replaced with reused member buffers.
  Deferred: groupshared tile for the pack firefly 3x3 loop (risk/benefit unfavorable — the loop is
  correct and the pass is not a measured bottleneck).

## P3 — Architecture

* [ ] `NrdConfig` singleton → AppFramework-owned member with per-frame snapshot handoff like
  `RenderConfig`. Interim state: the misleading comment is corrected, and all consumers sample
  each flag once per frame (`GPURenderer::Update`), which closes the known races.
* [ ] Move the enable/reset/readiness state machine from `GPURenderer` into `NrdDenoiser`;
  expose a single tone-mapping-input seam.
* [x] `RHINrdBackend` raw uint32_t enums: reviewed, no change — both sides of the seam compile
  against the same NRD headers, so a renumbering upgrade stays consistent in-tree; the Metal side
  switches on typed `nrd::Format` symbols. `GetPipelineCount()` is now used by the probe.
* [x] `DebugMode::HitDistance` retired from `RenderConfig` (with its shader case and
  `nrd_hitdist_test.py`; `NrdDebugMode` Diff/SpecHitDist views + nrd_inputs_test cover the channel).
* [x] `SkyLight::SetRotationYaw` removed (zero callers; every sky sample paid the rotation, and
  GPURenderer polled the proxy per frame for invalidation). Restorable from history if the
  feature is ever wired to an actual caller.

## P4 — Tests

* [x] Add a pitch/vertical-motion arm to the denoiser sweep gates (yaw-only hid the Y-flip bug).
  Done: `sweep_pitch_step_degrees` in DenoiserSweepTest, `nrd_motion_test --axis pitch`
  (gate 0.012, calibration in the docstring), wired into run_nrd_gates as gate 4.
* [x] `NrdProbeTest.mm` now drives `MetalNrdBackend::AddPipeline` directly (the duplicated
  SPIR-V→MSL→PSO chain is gone, so the probe cannot drift from the production path).
* [x] Shared `tests/nrd/nrd_common.py`: one sweep launcher, one image loader, one luminance fn;
  all six scripts refactored onto it (the stats fn became single-use once nrd_hitdist was removed).
* [ ] Fold the NRD gates into `dev/functional_test.py` / functional CI. Note: the two venv blocks
  in `action.yml` are mutually exclusive per runner (windows/glfw vs macos), so they never run
  twice on one machine — boilerplate only, fold when the gates move.

## P5 — Docs, comments, hygiene

* [x] `tmp/NRD_Plan.md` + `tmp/SVGF_Progress.md` untracked (local copies kept; `tmp/` gitignored);
  durable NRD notes distilled into `docs/Nrd.md` (linked from README); the
  `thirdparty/CMakeLists.txt` comment re-pointed there.
* [x] Comment style: change-narration in `nrd_pack.cs.slang` rewritten as the protocol fact;
  plan-phase labels and dated user-report citations removed from test docstrings.
* [x] `ComputeF0()` added to `pbr.h.slang` and used by all three F0 sites (incl. the NRD specular
  demodulation in `ray_trace.cs.slang`).
