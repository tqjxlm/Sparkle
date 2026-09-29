# Render Graph Review

Full review of branch `render-graph` (PR #100, draft) against `main` (merge-base b8b0274), covering Phases 0–2 as recorded in [RenderGraphProgress.md](RenderGraphProgress.md). Findings are action items; each has an ID, severity, location, the defect, and the proposed action. Verification: **C** = confirmed by tracing code, **P** = plausible, not traced end to end.

Status: review complete; fixes in progress. A finding marked **Done** is fixed in the commit that marks it; one marked **Closed** needs no change, for the reason given.

## Owner decisions

* **A4: no split.** PR #100 stays one PR.
* **Working notes stay.** `tmp/` stays committed on the branch while the work is in progress; the owner removes it at the end.
* **A1: consumers own samplers everywhere.** Samplers leave `RGTextureDesc` and the pool key (the graph), and leave `RHIImage` too (the RHI): every pass, material and bindless user supplies the sampler it samples with. The screen pass holds main's single upsample policy.
* **R6, R12: after the fixes.** The `PostChain` component with a template-method `Render()` and the shared raster-renderer base come after the correctness and trim work. Until then the design records the deviation.
* **A3: CI, with local pre-checks.** Before each push: `dev/check_format.py`, a glfw Release build with `--clangd`, and `dev/check_tidy.py`. Tests run in CI only.
* **M6/V7: remove** `SupportsPixelLocalRead`/`SupportsUnifiedImageLayouts` and the extensions they enable, until Phase 3 has a user.
* **R3: accepted.** The GPU and CPU pipelines do not cook the IBL; record it as a deviation.
* **T13: tests throw.** Under test, `RGCheck` throws instead of aborting; a `render_graph_errors` case catches one mistake per error class and compares the message.
* **T9: document now, remove later.** Correct docs/Denoiser.md and add a TODO entry to drop the SPIR-V 1.5 → 1.4 rewrite at the next NRD recook.
* **T19: keep** the 0.1 threshold: the 64 spp cases gate agreement with converged ground truth, not regressions.
* **G9, G10: record the gaps in the design as:**
  * deferred to Phase 3/4: transient mips, layers and cube; buffer accesses beyond copies; Copy blit, clear and mip generation; indirect dispatch; reader look-ahead folding; `GENERAL` collapse; the missing dump fields other than the resource type;
  * dropped until a user exists: `graph.Extract`, viewer mip/layer/channel selection, mandatory bindings, the `pool_reuse` kill switch;
  * implemented now: the dump resource-type field and the `full_barriers` kill switch.
* **A2: rename** to `DontCare`, no decision needed.
* **A1 details:**
  * each screen pass chooses its own filtering: the policy is a per-pass choice, not one rule in `ScreenQuadPass`;
  * whether a format can be filtered linearly is asked of the device (`RHIContext`), not read from a fixed list;
  * the sky box keeps each map's exact sampler (`GetSkyBoxMap` returns the image with its sampler);
  * shared sampler constants live on the class that owns the resource (`SkyRenderProxy::SkyMapSampler`, `ImageBasedLighting::MapSampler`);
  * the pre-existing sampler issues the A1 study found are TODO entries;
  * `RenderGraph` takes its own `RHIContext *` in the same change (G12).
* **V2: keep** seeding swap chain images with a `Present` access.
* **Follow-ups:**
  * the GPU pipeline's `RGBAFloat` accumulator upsamples bilinearly where the device filters it linearly;
  * Present samples nearest when the back buffer is an integer multiple of Screen, bilinear otherwise;
  * the path tracer's next event estimation samples the sky map only when there is one (fix it);
  * `RenderGraph::ReadOnHost` stays a separate call, and each host-read barrier is recorded right after the buffer's last pass;
  * `NativeAccess` on Compute passes and the graph resetting bind state after raw recording are dropped until a user needs them;
  * the `subpass` TODO entry stays until on-chip passes are implemented;
  * every push to PR #100 is followed by watching its CI.
* **V9 follow-ups:** buffer uploads keep recording into the open context before the graph; `BeginCommandBuffer()` returns the context it opens; `GetCommandContext()` asserts it is not called while a graph executes.

## 0. Answers to the review questions (summary)

| Question | Verdict |
| --- | --- |
| Design questions answered reasonably? | Mostly yes; D1–D22 are well argued. Two exceptions. Sampler policy regressed a previously settled design (A1). The `None` load/store naming conflicts with the Phase 3 plan (A2). |
| Better architecture? | Consumer-owned samplers (A1). A `PostChain` component and a template-method `Render()` (R6). A shared raster-renderer base for Forward/Deferred (R12). Pass contexts as the only recording path (R7, V9). |
| Better workflow? | Build and gate locally before pushing (A3). Split the 11K-line PR (A4). |
| Implemented as designed? | Largely. Recorded deviations are justified. Unrecorded ones: G9, R3, R6, R7. Most should update the design; R6/R7 should change the code. |
| Surgical? | Yes, with small leftovers: M6/V7 (speculative capabilities that enable real extensions), V8, R8, T30. |
| Regressions? | One confirmed: the Metal timer reports 0 ms for a pass recorded twice (M1). Plausible: R1 (screenshot with a hidden UI never completes), M2/R2 (MetalFX stale frame), R4 (null renderer), V1 (dropped-frame state). Latent: G1 (`FullyOverwrites` on read-write discards). No visual regression found in any renderer. |
| Comments minimal? | Mostly yes. There is no change narration. A few comments restate names or are inaccurate (G25, G26, R16, V17), and some invariants lack a comment (G27, V17). |
| Docs reader-oriented? | No: docs/RenderGraph.md is a dense spec with rotting per-renderer tables (T1–T8). |
| Public APIs to local scope? | Yes: A6, G11–G14, M7, R9–R11, V10. |
| Modularity / reuse? | Candidates: R12, R13, G15–G19, T23, T24, V14, M10. |
| Modern C++? | Mostly modern C++20. Remaining: double `std::function` erasure (G20), unconstrained templates (G21), constexpr (G22), `std::span` (V16). |

## 1. Architecture and workflow

* **Done** (screen passes choose their filter; `RHIContext::SupportsLinearFiltering` decides whether a format may be filtered bilinearly; images carry no sampler). **A1 [high, C] Sampler policy moved back to producers, a design a previous review rejected.** Behavior is unchanged (the renderer review traced it to equal main); the problem is where the policy lives. `RGTextureDesc::sampler` (`RenderGraph.h:131`) and `RGBuilder`'s sampler binding bind the sampler the *image* carries, so the upsample policy moved from `ToneMappingPass::GetInputSampler` (main) into `Renderer::GetSceneColorDesc` (`Renderer.cpp:210`), `CPURenderer.cpp:31` `GetImageDesc`, and per-image samplers in `GPURenderer.cpp:87` and `NrdDenoiser.cpp:143`. The sub-resolution review had settled on a single consumer-side policy because producer-baked samplers triplicated it and caused Repeat-wrap edge fringes. The sampler is also part of the transient pool key (`RGTexturePool.h:25`, `RenderGraph.cpp:628`), which blocks aliasing and cross-pipeline reuse between textures that differ only in how a consumer samples them. Action: samplers are not graph resources and need no synchronization. Remove `sampler` from `RGTextureDesc` and the pool key; let the consumer supply the sampler (an `RHISampler` argument on the sampler-binding overload, or the pass binds its own sampler as it binds UBOs); restore one upsample policy in the screen pass (including the `RGBAFloat` no-linear-filter exception).
* **Done.** **A2 [med, C] `RHILoadOp::None` / `RHIStoreOp::None` mean "don't care".** `RHIRenderingInfo.h:13-24`. Vulkan's `LOAD_OP_NONE`/`STORE_OP_NONE` mean "leave memory untouched", and design §6.5 plans `StoreOp::None` → `STORE_OP_NONE` for read-only depth in Phase 3. With the current naming, that lowering change would silently turn every "no later reader" attachment (`RenderGraph.cpp:915`) into `STORE_OP_NONE`. Action: rename to `DontCare` now; add a distinct `None` only when Phase 3 needs it.
* **Done.** **A3 [high] Workflow: nothing is built or run locally.** RenderGraphProgress.md:148 says so, which contradicts the standing local-gates rule (check_format → check_tidy → full run_tests before push). The one local physical-GPU run (M5 Max) is what found the Metal skipped-encoder timer bug, and CI cannot exercise Metal timing, NRD or MetalFX at all. Action: every step builds and runs the local gates on macOS (macos + glfw/MoltenVK) before pushing; CI covers the platforms that can't run locally (Windows, Linux lavapipe, Android emulator). Update the Workflow section.
* **Closed (owner: no split).** **A4 [med] PR shape.** PR #100 is ~11K insertions and 5.7K deletions; `0ca0123` alone ("Phase 0+1") touches 133 files, +4.9K/−3.5K. Action: split into reviewable PRs merged in order: (1) Phase 0 RHI (`cc35c0e`..`8c98057`, independently valuable: sync2, dynamic rendering, sync-validation gate, several real hazard fixes); (2) graph core + tests; (3) renderer ports; (4) Phase 2 visibility. Split `0ca0123` back into its step commits (1.2–1.11) if the history still allows it.
* **Done.** **A5 [low] Stale design header.** `RenderGraphDesign.md:3` says "Nothing here is implemented", but Phases 0–2 are. Action: state which phases are implemented and point to the progress log for deviations.
* **Done.** **A6 [low, C] Binding type spelled three times.** `std::function<RHIMemberBinding(RHIContext *, RHIImage &, const RGSubresources &)>` appears at `RenderGraph.h:296` (as `RGBuilder::ImageBinding`), `:540` and `:572`. Action: one alias shared by `RGBuilder` and `RenderGraph`.
* **A7 [info] Size is within target.** The core is 2,241 physical lines, but about 1,276 once blanks, comments and brace-only lines are excluded; that is inside the design's 1,000–1,500. Not over-engineered: the per-mip runs, reason strings, placeholders and binding validation all have users. Action: the dead API (G11) and DRY merges (G15–G19) trim roughly another 150 lines.
* **Done.** **A8 [low, C] Binding check depends on list order.** `CheckBindingsApplied` (`RenderGraph.cpp:1030`) walks `pass.bindings` by index, which relies on `ResolveBindings` (`:694`) appending textures, then buffers, then placeholders in that exact order. Action: store the resource name (or access index) with each resolved binding.
* **Done.** **A9 [low, C] Buffer planning calls the image rule with fake layouts.** `RenderGraph.cpp:735` wraps accesses in `RHIImageState{Undefined, access}` to reuse `TransitionImageState`. Action: expose the access-only rule from `RHIBarrier`/`RHIImage` and have the image rule call it.
* **Done** (commented next to the loop). **A10 [info] Culling works per texture, not per subresource.** `Cull` (`RenderGraph.cpp:557`) keeps one `needed` flag per texture. Correct today because transients have one mip and one layer and imports always keep their writers. Action: keep it, and note the constraint next to the loop if multi-mip transients arrive.

## 2. Graph core

The compiler is correct for every graph the renderers build today. No high-severity defect was found. Verified correct:

* transient lifetimes and aliasing (strict `last_pass < first_pass`; planned states keyed by physical image, so a reusing transient starts with a discard barrier);
* RAW, WAR and WAW, layout changes, read→read in a new stage, and per-subresource seeding and mip runs;
* store inference;
* buffer and AS barriers, including build → build;
* the External contract.

Correctness (latent):

* **Done.** **G1 [med, C] `FullyOverwrites` means two things.** For barriers it discards every written access of the pass (`RenderGraph.cpp:763-765`), including `StorageReadWrite`. For culling and store inference (`UsesContents`, `:144-147`), reading accesses still use their contents. Scenario: a `FullyOverwrites()` compute pass with `StorageReadWrite(history_import)` discards history on Vulkan (`from_layout = Undefined`). Action: define discard through one rule, `discard = !UsesContents(...) || (write && !writer && !imported)`.
* **Closed** (unreachable: `RHIImage` has no 2D array type and transients have one layer; the view type comes with transient layers, G9). **G2 [low, C] Storage view type misses 2D arrays.** A storage view is `Image2DArray` only for cubes (`RenderGraph.cpp:311-317`), so a multi-layer non-cube storage binding builds an invalid `Image2D` view. Action: use `layer_count > 1 || cube`.
* **G3 [low, C] Culling keeps unneeded writers alive.** A partial write always sets `needed = true` (`RenderGraph.cpp:580-583`), even when nothing later needs the texture. This keeps earlier writers alive (extra work, never wrong output). Action: `needed = ReadsContents || (needed_after && !(clear || fully))`.
* **Done.** **G4 [low, C] Only External passes are contract-checked.** A Raster, Compute or Copy `record` that calls `Transition`/`Upload` on a declared image desyncs the tracker silently (`RenderGraph.cpp:1012`). Action: run `CheckExternalContract` for every kind; it is cheap.
* **Done** (see V1). **G5 [low, P] Unsubmitted command buffers leave stale tracked state.** When a recorded frame is never submitted (surface loss), imports' tracked states describe transitions that never ran. This is the same class of problem `RHIImage::Transition` has. Action: note it; consider resetting imports' tracked state on a skipped submit.
* **G6 [low, C] Wrong load reason on imports.** `last_writer` is per texture (`RenderGraph.cpp:762,797`), so a mip-1 write makes a later mip-0 attachment report "written by X". The load op itself is right.
* **G7 [low, C] Pooled images keep their first name.** A pooled image keeps the debug name of the transient that created it (`RenderGraph.cpp:654`), so reused images are mislabelled in captures. Action: rename on acquire, or name by physical slot.
* **G8 [low, C] Barrier splitting ignores layer uniformity.** Non-uniform accesses split per layer first (`RenderGraph.cpp:879-894`), so a cube whose mips differ but whose layers match gets 6× the barriers. Performance only. Also, one memory barrier is emitted per buffer access (`:742`); merge them per pass.

Deviations from the design not recorded in the progress log. For all of these, update the design to match the code, except where an action says otherwise.

* **Done** (recorded in the design, §16). **G9** Missing from the implementation:
  * transient mips, layers and cube type;
  * `graph.Extract` (drop it from §4.1 until a user exists);
  * buffer accesses beyond `CopySrc`/`CopyDst` (no uniform, vertex, index, indirect or storage access; no transient buffers; binding a graph buffer aborts);
  * `NativeAccess` on Compute; the graph resetting bind state after External passes;
  * Copy blit, clear and mip generation; indirect dispatch;
  * reader look-ahead folding (§6.6); unified-layout `GENERAL` collapse (§6.6, although the capability is queried);
  * dump fields: backward-dependency flags, dependency levels, CPU record ms, byte totals, resource type;
  * kill switches `pool_reuse` and `full_barriers`;
  * viewer mip, layer and channel selection;
  * mandatory bindings (declared bindings are optional).
* **Done** (dump `type` field and `render_graph_full_barriers`: one memory barrier from every earlier access per pass). **G10** Better than designed: `RGAccelerationStructure` is its own handle type (D17 names only `RGTexture`/`RGBuffer`). Action items worth doing now: add the dump resource-type field, which removes the viewer's "textures first" assumption (T26), and the `full_barriers` kill switch for bisecting synchronization bugs.

API scope and encapsulation:

* **Done.** **G11 [low, C] Dead API.**
  * `RGBuilder::StorageRead` (both overloads, `RenderGraph.h:179,217`) and `RGRasterContext::GetAttachmentSignature` (`:378`) have no callers. Action: delete them.
  * `RGExternalContext::GetCommandContext` (`:439`) has no callers either. Keep it: route NRD (`NrdDenoiser.cpp:443,473`) and MetalFX (M9) through it instead of `rhi_->GetCommandContext()`.
* **Done.** **G12 [low] Access narrower than public.**
  * Make protected: `RGPassContext::GetBuffer` and `GetAccelerationStructure` (only `RGCopyContext` uses them).
  * Make private: `RGTexturePool::Key`.
  * Move into the .cpp: `RGSubresources::Overlaps`/`Contains` (used only there).
  * `friend class RenderGraph` in `RGPassContext` (`RenderGraph.h:358`) is likely redundant (P, needs a compile).
  * The graph reads the pool's private `rhi_` through friendship (`RenderGraph.cpp:703,712`); give the graph its own `RHIContext *`.
* **Done** (private `RenderGraphInternal.h` plus a non-template `SetRecord`). **G13 [med/low] Internals in the public header.** `Access`, `BufferAccess`, `Pass`, `Texture` and `Buffer` (`RenderGraph.h:529-624`, ~100 lines) are in the header only because the `AddPass` template writes `passes_[index].record`. Action: a non-template `SetRecord(index, std::function<void(RHICommandContext &)>)` (or pimpl) moves all of them to the .cpp; also fixes A6.
* **Done** (a `std::variant` of the buffer and TLAS references: the binding needs the typed TLAS). **G14 [low] `Buffer` is a union tagged by null pointers.** `RenderGraph.h:609-623`, `.cpp:686`. Action: store the tracked access pointer plus a kind (which also feeds the dump type field).

Reuse / DRY:

* **Done.** **G15** Lifetime accumulation is written twice (`RenderGraph.cpp:590-602`, `670-684`) and dumped twice (`RenderGraphDump.cpp:206-211`, `225-230`). Action: one `Lifetime { optional first; last; Extend(pass) }`.
* **Done.** **G16** The slot → attachment dispatch appears three times (`RenderGraph.cpp:809-825`, `947-954`; `RenderGraphDump.cpp:161-163`). Action: store `load_op`/`store_op` on `Access` with the reasons, and build `rendering_info` once after store inference. This also takes attachment assembly out of `PlanBarriers`.
* **Done.** **G17** Duplicated checks and lookups:
  * default-stage resolution (`RenderGraph.cpp:198,287`): one `ResolveStages(kind, stages)`;
  * the kind and declared-twice checks in `Declare`/`DeclareBuffer` (`:171-179`, `257-262`);
  * declared-lookup in `GetImage`/`CheckDeclared` (`:350,370`).
* **Done** (usages now print in bit order; goldens updated). **G18** The dump's hand-written enum name tables (`RenderGraphDump.cpp:27-70`) already miss `SRV` and `TransientAttachment`. Action: `magic_enum::enum_flags_name` with `is_flags`.
* **Closed** (the overloads already share `Bind`; merging further needs member pointers). **G19** The four bound-access overload templates (`RenderGraph.h:185-238`) can share one private helper.

Modern C++ (C++20):

* **Done.** **G20 [med/low, C] Double type erasure.** `AddPass` (`RenderGraph.h:631-636`) wraps the execute lambda in `std::function<void(Context &)>` inside another `std::function`. Action: capture the returned callable directly.
* **Done.** **G21 [low] Unconstrained templates.** `template <typename Setup>` has no constraint. Action: `requires std::invocable<Setup, RGBuilder &> && std::invocable<std::invoke_result_t<Setup, RGBuilder &>, Context &>` for a readable error when the context type is wrong.
* **G22 [low] Constants could be constexpr.** `RenderGraph.cpp:15-21` are `static const` because the `RegisterEnumAsFlag` operators are not constexpr. Action: make them constexpr; `KindAllows` follows.
* **G23 [low] Initializer noise.** Default member initializers (`= {}`) on the vector/string fields of `Access` and `Pass` remove the `.bindings = {}, .barriers = {}, …` noise at `RenderGraph.cpp:181-191, 504-512`.
* **G24 [low] Map where a vector fits.** `std::unordered_map<const RHIImage *, …>` (`RenderGraph.cpp:722`). Action: a vector indexed by physical image or import.

Comments:

* **G25** Inaccurate comments:
  * `RenderGraph.h:115` says size classes resolve "when the graph compiles"; they resolve in `CreateTexture`.
  * The `// compiled` marker on `Texture` (`RenderGraph.h:595-601`) covers fields set at `Import`/`CreateTexture`.
* **G26** Comments that restate the name: `RenderGraph.h:292,295,324,327`; `RenderGraph.cpp:18`.
* **Done.** **G27** Missing invariants:
  * the positional coupling between bindings and checks (A8);
  * per-texture `needed` relying on single-subresource transients (A10);
  * planned states shared by aliased transients (`RenderGraph.cpp:722`).

## 3. RHI and Vulkan

Traced and correct:

* sync2 lowering (write-only source masks, read→read chaining, `TRANSFER` for blits, AS build accesses) and the sync1 android-emulator fallback;
* the acquire-semaphore ring;
* deferred descriptor-set release timing;
* the bindless dirty-clear fix (a real bug on main);
* TLAS `ALLOW_UPDATE` + scratch `max(build, update)`;
* timer valid-bits masking and `read_frame_`;
* PSO cache lifetime;
* `colorAttachmentCount` with `UNDEFINED` gaps;
* the viewport.

Correctness:

* **Done** (a recorded frame is always submitted; the lost surface only fails the present). **V1 [med, P] A dropped frame leaves CPU-side state ahead of the GPU** (also G5). When `!CanRender()`, a fully recorded frame is discarded (`VulkanContext.cpp:333-338`, `RenderFramework.cpp:404-413`). Its CPU-side commitments stay:
  * the tracked image states the graph wrote through;
  * `RHITLAS::staged_`, which `RecordBuild` cleared (`RHIRayTracing.h:74-78`);
  * timers left waiting on queries whose reset never ran.

  Scenario: surface loss on the frame that recorded the first TLAS build. Later frames refit (`MODE_UPDATE`) a structure that was never built, and tracked layouts are ahead of the GPU, so the next barrier has the wrong oldLayout. The failure class exists on main; this branch formalizes the drop. Action: on a dropped frame, reset imported/pooled tracked states to `Undefined`, re-stage the TLAS build, and reset the frame slot's timers.
* **Done** (swap chain images start with a `Present` access, so their first use chains to the acquire as well). **V2 [low, P] Barriers from `Present` don't wait for the acquire.** `RHIAccess::Present` has no stage mapping (`VulkanCommandContext.cpp:42-74`), so a barrier from Present gets `srcStage = NONE` and doesn't chain to the acquire wait. Only the graph works around it (`RenderGraph.cpp:850-856` adds the attachment's own access). Action: map a Present *source* to `COLOR_ATTACHMENT_OUTPUT` in the RHI and drop the graph special case.
* **V3 [low, P] Mid-frame submit nulls the frame context.** `SubmitCommandBuffer` while the frame context is active sets `command_context_ = nullptr` (`VulkanContext.cpp:719-733`). This pre-exists, and no caller does it today. Action: assert it.
* **V4 [low, P] Recompiles keep stale pipelines.** The per-signature PSO cache is never invalidated (`VulkanPipelineState.cpp:383-391`): a second `Compile()` after changing blend/depth keeps the old pipelines and leaks the old layout. No caller recompiles. Action: clear the cache in `Compile()`, or assert single compile.
* **Done.** **V5 [low, P] Pass state is not asserted.** `DrawMesh`/`DispatchCompute` don't assert an open rendering/compute pass (`RHICommandContext.cpp:85-101`). `BeginVulkanRendering` doesn't check attachments are in their attachment layouts (`VulkanRenderPass.cpp:16-72`), though RHICommandContext.h:30 states that contract. Action: add both asserts in common code.
* **V6 [low, C, pre-existing] TLAS instance-buffer bugs.** `VulkanRayTracing.cpp:144-152`:
  * `RecreateBuffer` rounds the size up to a power of two, and `UploadImmediate` then copies `attribute_.size` bytes from `instances`, reading past the end of the vector.
  * `VulkanTLAS::Update` writes the host-visible instance buffer while the previous frame's refit may still read it.

  Action: TODO entries (the second is already noted in the progress log).

Surgical / dead code:

* **Done.** **V7 [med, C] Unused capabilities enable real extensions.** `SupportsPixelLocalRead`/`SupportsUnifiedImageLayouts` have no callers (`RHI.h:118-122`), yet `VulkanContext.cpp:945-968, 1025-1046` enable `dynamic_rendering_local_read` and `unified_image_layouts` on every device that has them, which is driver risk for no benefit. Action: remove until Phase 3 (with M6).
* **Done** (the `VulkanRHI.h` reorder no longer exists). **V8 [low, C] Leftover declarations.**
  * `RHIPipelineStage::DrawIndirect/VertexInput/VertexShader/EarlyZ/LateZ` are referenced only by the legacy translation switch, and `TransitionRequest::discard` only by tests (`RHIImage.h:18-31`).
  * `ImageUsage::TransientAttachment` has no Vulkan user left (`RHIImage.h:117`).
  * The `msaa` config and `VulkanContext::msaa_samples_` are write-only (`RHIConfig.cpp:22`, `VulkanContext.cpp:792`).
  * `VulkanContext::GenerateMipmaps`/`CopyBuffer` are declared but undefined (pre-existing, `VulkanContext.h:186-188`).
  * `VulkanRHI.h` reorders existing declarations (diff noise).

  Action: delete what the branch orphaned, and revert the reorder.

API scope:

* **Done** (image transitions and uploads take the recording context; buffer uploads stay in the frame prologue and use the open context; `RHI.h` documents who may call `GetCommandContext`). **V9 [med, C] Recording still goes through the global context.** Besides the denoisers (R7):
  * `RHIBuffer::Upload`/`PartialUpdate` (`RHIBuffer.cpp:98,294`);
  * `VulkanImage::Upload`/`UploadFaces`/`Transition` (`VulkanImage.cpp:70,101,198`);
  * `RHIImage::ReadToMemory`, `IBLPass::Finalize`.

  The legacy `Transition` virtual is the main reason `GetCommandContext()` survives. Action: `Transition` and uploads take a context; then `RHIContext::GetCommandContext()` can shrink to frame setup.
* **Done** (`VulkanRenderPass.{h,cpp}` folded away). **V10 [low] Members that could be private or go.**
  * `RHIPipelineState::ApplyBinding` (one caller) → private + friend.
  * `RHIBuffer::GetUsageAccess` → protected.
  * `RHICommandContext::DrewOrDispatched` is `!GetPipelines().empty()` with one caller → drop.
  * `BeginVulkanRendering` → static in `VulkanCommandContext.cpp`.
  * `GetVkPipelineRenderingCreateInfo` → next to the PSO code.
  * `VulkanRenderPass.{h,cpp}` no longer holds a render pass → rename or fold.
* **Done.** **V11 [low] Pointer vs reference contexts.** `RHIUiHandler::Render(RHICommandContext *)` and `RHINrdBackend::RunDispatches(RHICommandContext *, …)` take pointers, while timers and `RecordBuild` take references. Action: references everywhere.
* **Done** (Metal records labels as command buffer debug groups). **V12 [low] Copy and External passes get no debug label**, because `BeginDebugLabel` is protected. That misses the "labels always on" goal for `BuildTLAS`, `Upload` and `Readback`. Action: let the graph label every pass.
* **Done.** **V13 [low] Compute passes take barriers differently from raster passes.** `BeginRendering` takes the barrier batch, but compute calls `Barrier` after `BeginComputePass` (`RenderGraph.cpp:999-1000`). Action: `BeginComputePass(pass, barriers)`.

Reuse:

* **Done.** **V14 [low]**
  * The conservative "usage → X → usage" buffer barrier pair is written twice (`VulkanBuffer.cpp:90-100`, `RHIBuffer.cpp:100-112`).
  * `Barrier({}, std::span(&b, 1))` appears 7 times: add a single-barrier overload.
  * `VulkanNrdBackend.cpp:333-366` hand-builds `VkImageMemoryBarrier2`/`VkDependencyInfo`: share the `BarrierInternal` helper.
  * Headless and windowed frame begin/end are duplicated (`VulkanContext.cpp:429-438/504-510`, `518-521/538-541`).
* **Done** (copies record nothing; `RenderGraph::ReadOnHost` plans the host read of readback buffers). **V15 [low] Copy synchronization is inconsistent and undocumented.**
  * `CopyBuffer` self-synchronizes with usage barriers.
  * `CopyImageToBuffer` adds only HostRead.
  * `CopyBufferToImage`/`BlitImage` add none.
  * Copies hardcode `TRANSFER_*_OPTIMAL` while blit uses the tracked layout.

  Action: pick one rule (the graph synchronizes; copies record nothing extra) and document it in `RHICommandContext.h`.

Modern C++ / comments:

* **Done.** **V16 [low] Modern C++.**
  * Pointer+count parameters → `std::span` (`VulkanCommandContext.h:48,117`).
  * `GetSync1Stages` narrows silently: assert on unhandled high bits (`VulkanCommandContext.cpp:92`).
  * The one-shot scope's raw `new`/`delete` → `std::optional` (`VulkanContext.cpp:712`, pre-existing).
* **Done.** **V17 [low] Comments.**
  * The acquire comment argues a counterfactual (`VulkanContext.cpp:452-454`); one line suffices.
  * `VulkanRenderPass.h:43` repeats `RHIRenderingInfo.h:29`.
  * "universal queue" should say graphics queue (`VulkanContext.h:115`).
  * Missing invariants: why source masks are write-only (`VulkanCommandContext.cpp:14,85`); that Present has no stage (V2); the per-draw `dynamic_cast` + `std::function` cost of `RHIMemberBinding` (`RHIShader.h:330-356`).

## 4. Renderer port

No high-severity regression. The following match main when traced:

* pass order per renderer (TLAS build and IBL cook moved into the graph without changing their relative order);
* clears (IBL once per map through `cleared_`);
* formats; the samplers the images carry (behaviorally identical to main's `GetInputSampler`, though see A1 on where the policy lives);
* the placeholders for a missing shadow map or IBL;
* sky-map selection (Deferred's sticky override is fixed);
* exposure upload;
* the GPU clear gate, dynamic spp and seeding;
* the accumulator clear after recreation (`pixels_dirty_ = 1`);
* ImGui dump-publish lifetime;
* execute-lambda lifetimes.

Several latent bugs are fixed on the way: stale camera/light pointers in `DirectionalLightingPass`, `SetIBL(nullptr)`, and the unchecked BRDF bind.

Behavior:

* **Done.** **R1 [low, P] Screenshot request can hang.** A screenshot "with UI" requested on a frame where the UI is hidden never completes: the readback is added only inside `if (render_ui && ui_pass_)` (`Renderer.cpp:314-319`). `screenshot_saving_` stays set, and Save stays disabled until another request replaces it. Main fell back to a readback without UI (except CPU). Action: fall back to the pre-UI readback when the UI does not draw.
* **Done** (see M2). **R2 [low, P] MetalFX may show stale output.** Same finding as M2.
* **Done.** **R3 [low, C] GPU/CPU pipelines no longer cook the IBL.** The GPU IBL cook now runs only inside Forward/Deferred graphs; `SkyRenderProxy::Update` is gone. So under the GPU/CPU pipelines the IBL never cooks, and its artifact callback no longer persists the cooked map. Those pipelines don't use the IBL, but this isn't recorded. Action: record it in the progress log, or decide that a map cooked in any pipeline should persist.
* **Done** (the named `RequestGraphDump` and the save-screenshot tasks stay unguarded: skipping them would leave their request pending). **R4 [low, P] Null dereference at startup.** The graph-panel render-thread task dereferences `renderer_` without the null guard `NotifySceneLoaded` has (`RenderFramework.cpp:493, 567-568`). Opening the tab before the renderer exists crashes. Action: guard it.
* **R5 [info, C] Cook task unregisters a frame later.** `ibl_cook_pending_` is checked before this frame's `AddCookPasses` (`ForwardRenderer.cpp:46-60`), so the async task unregisters one frame later than on main. Harmless.

Design conformance:

* **Done** (`PostChain` component; `Renderer::Render` calls each renderer's `BuildGraph`). **R6 [med] Post chain lives in the base class, not as a component (§5).** Design §5 has a free `AddPostChain(g, PostPasses &, scene, FrameFlags)` and a per-renderer `BuildGraph(g)`. The code makes the post chain part of the `Renderer` base class instead: `ui_pass_`, `present_pass_`, `graph_view_pass_`, the view state, `screen_desc_` and the timers (`Renderer.h:127-156`). Every `Render()` repeats create graph → `AddPostChain` → `ExecuteGraph`. Action (preferred): a `PostChain` component, plus a non-virtual `Renderer::Render()` that calls a virtual `BuildGraph(graph)` returning the scene texture and screen pass. The tone mapping pass, created and updated identically in three renderers (Forward :32/:98, Deferred :39/:106, GPU :106/:433), moves into it. Otherwise record the deviation.
* **Done.** **R7 [med, C] Denoisers record through the global command context.** `Encode` takes `const RGPassContext &` and then uses `rhi_->GetCommandContext()` (`NrdDenoiser.cpp:373,443,473`; `MetalFxDenoiser.mm:499,521`). This goes against D3. Action: take `RGExternalContext &` and pass its context down to `RenderReblur`/`RunDispatches`; covers G11 and M9.

Surgical / dead code:

* **Done** (`CPURenderer::Update` is inline: the base declares it pure virtual). **R8 [low, C] Leftovers.**
  * Orphan includes: `CPURenderer.h:8` (`rhi/RHIImage.h`), `ForwardRenderer.cpp:10` (`CameraRenderProxy.h`).
  * `CPURenderer::Update` is now an empty out-of-line body (`CPURenderer.cpp:76-78`).
  * `ScreenQuadPass::SampleInput` is virtual only because of `ToneMappingPass`'s table type, and `DirectionalLightingPass` inherits it unused and hides the base `AddTo` (`ScreenQuadPass.h:61`, `DirectionalLightingPass.h:22`). Acceptable; composition would be cleaner.

Encapsulation:

* **Done.** **R9 [med, C] Base-class members wider than needed.**
  * `ui_pass_` and `present_pass_` (`Renderer.h:153-156`) are protected, but no derived class uses them. Make them private.
  * `screen_desc_` can be private too: derived classes read only what they just passed to `InitPostChain`.
* **Partly done** (`Renderer.h` still includes `RGTexturePool.h`). **R10 [low] Helpers on the wrong class.**
  * `ToneMappedScreenDesc` belongs on `ToneMappingPass`.
  * `SceneDepthDesc`, `GetSceneColorDesc` and `GetSkyBoxMap` are raster-only (see R12).
  * `CreateScreenshotBuffer` only uses `rhi_`: anonymous namespace.
  * `Renderer.h` includes `RGTexturePool.h` where a forward declaration suffices.
* **R11 [low] Two `RequestGraphDump` overloads with different semantics.** One writes a file; the other is a one-shot callback that silently replaces a pending one (`Renderer.h:58-62`). Action: keep only a list of one-shot consumers and let `RenderFramework::RequestGraphDump(name)` write the file. That removes `graph_dump_path_`, `graph_dump_completion_` and the FileManager dependency from `Renderer`, and a test dump and the UI panel can coexist.

Reuse / DRY:

* **Done** (`RasterRenderer` base with scene-pass hooks). **R12 [med, C] Forward and Deferred differ only in their scene passes.** Their shared parts are now identical:
  * `HandleSceneChanges` (`ForwardRenderer.cpp:101-142` = `DeferredRenderer.cpp:109-150`);
  * the `ibl_cook_pending_` block (`:46-53` = `:51-58`);
  * the graph prologue (cook, shadow, `LightingInputs`) and the sky-box tail;
  * half of `InitRenderResources`/`Update`, and six members.

  The only difference is BasePass vs GBuffer + Lighting. Action: an intermediate raster-renderer base (or shared component), which also takes the raster-only helpers from R10.
* **R13 [low] IBL cook passes share a skeleton.** `IBLBrdfPass.cpp:94`, `IBLDiffusePass.cpp:100`, `IBLSpecularPass.cpp:64` all do batch → UBO upload → `AddComputePass` with one storage binding → sample bookkeeping. Action: an `IBLPass` helper taking the table-specific declare lambda and dispatch size (~15 lines each).

Modern C++ / comments:

* **R14 [low]** `GPURenderer.cpp:161` captures `&denoiser_inputs` in the setup lambda, which is safe only because setup runs synchronously. Capture it by value.
* **R15 [low]** `RenderFramework.cpp:569` deep-copies the whole dump JSON every frame while the panel is open. Move it instead.
* **R16 [low] Comments.**
  * These restate the code: `ScreenQuadPass.h:76`, `MetalFxDenoiser.mm:579`.
  * The `AddPostChain` doc (`Renderer.h:130-132`) repeats the pass list that docs/RenderGraph.md also has.
  * `TestCase.h` uses `///`, where the codebase uses `//`.

## 5. Metal backend

Encoder lifetime, descriptor lowering, the per-signature PSO cache, TLAS staging, the headless back buffer and ImGui ordering were reviewed and are correct. ARC use is fine.

* **Done** (the test compares a pass recorded twice with one recorded once: MoltenVK measures 0 for both). **M1 [med, C] A pass timed twice in one command buffer reports 0 ms.** `MetalTimer.mm:112-138`: each `End` adds a completed handler to the same command buffer. Handler 1 computes the time and stores the samples in `previous_samples_`; handler 2 then finds every sample equal to `previous`, treats it as unwritten, and overwrites the result with 0. This breaks RHIPass's "reports its last run" contract and affects `RGPassTimers` passes that share a name. `pass_timestamps` records a pass twice per frame but accepts 0, so it hides the bug. A second handler can also re-set `resolved_` after `TryGetResult` ran (P). Action: register one handler per command buffer (remember the pending buffer); make the test reject 0 on devices that support timestamps.
* **Done** (`AddTo` checks the inputs and returns the accumulator). **M2 [low, P] MetalFX output can stay unwritten for a frame.** `MetalFxDenoiser.mm:473-506, 538-542`: `AddTo` chooses and imports the displayed output at build time. If `Encode` then fails (`BindInputs` fails, no scaler), it only clears `ready`, and tone mapping samples stale contents. Action: decide feasibility in `AddTo`, or fall back to the accumulator when the encode fails.
* **Done.** **M3 [low, C] Empty acceleration-structure encoder.** `MetalRayTracing.mm:162-165, 205`: `Build()` opens and ends an encoder even when no BLAS is dirty. Action: open it only when there is BLAS work.
* **Done** (asserts where no pass context is available). **M4 [low, P] Null command context dereferenced.** `MetalRayTracing.mm:162`, `MetalRHI.mm:131`, `MetalFxDenoiser.mm:521` dereference `GetCommandContext()`, which is null outside a command buffer. No current path reaches this, and the old code silently messaged nil. Action: assert, or take the context as a parameter (see M9).
* **Done** (common `Compile` asserts a signature for graphics pipelines). **M5 [low, C] Undocumented Metal requirement.** Metal asserts that an attachment signature is set before `Compile` (`MetalPipelineState.mm:355`), while `RHIPIpelineState.h` documents it as optional. Metal also ignores `samples` (no `rasterSampleCount`); this is harmless while the graph is single-sampled. Action: state the requirement in the common header, or make Vulkan require it too.
* **Done.** **M6 [low, C] Speculative API.** `SupportsPixelLocalRead` and `SupportsUnifiedImageLayouts` (`MetalRHI.mm:85-89`, `MetalRHI.h:24-27`, and the Vulkan and common counterparts) have no callers. Metal returning false for unified layouts is also semantically backwards. Action: delete until Phase 3 needs them.
* **Done.** **M7 [low] Backend-only calls are public.** `MetalCommandContext::Begin/End` (`MetalCommandContext.h:29-31`) are called only by `MetalContext`. Action: make them private and friend `MetalContext`.
* **Done.** **M8 [low] Lazy cache for a constructor-time fact.** The pass-timestamp support flag (`MetalContext.mm:12-19`) is a lazily cached `std::optional<bool>`, although it can be computed once in the constructor. Action: compute it there; the getter becomes const.
* **Done.** **M9 [low, C] MetalFX reaches for the global context.** `MetalFxDenoiser::Encode` takes `const RGPassContext &` and then calls the global `GetCommandContext()` (`MetalFxDenoiser.h:31`, `.mm:521`). Action: take `RGExternalContext &` and use its `GetCommandContext()`.
* **Closed** (a shared helper saves ~4 lines per backend and needs uncompiled Metal edits). **M10 [low] Duplicated signature cache.** The find-or-emplace pipeline cache is written twice: `MetalPipelineState.mm:398-414` and `VulkanPipelineState.cpp:218-226`. Action: optional shared helper.
* **Done.** **M11 [low] Vague comment.** `MetalImage.h:33`, "the state is still tracked to evolve as on Vulkan". Action: "tracks the state the graph plans from; records nothing".

## 6. Docs

* **Done.** **T1 [high] docs/RenderGraph.md is a spec, not a manual.** Paragraphs at :26-54 run 1,000–1,700 characters. The binding rules (:31), the abort list (:45) and the barrier rules (:49) are each one sentence chain. Action: restructure per the outline below, one rule per bullet.
* **Done.** **T2 [high] Per-renderer pass tables duplicate the goldens.** :56-124 repeat `tests/render_graph/golden/*.txt` and will drift. They also list passes no golden confirms (`Ui`, both `Readback`s, `ClearAccumulator`, `BuildTLAS`). Action: one short paragraph per renderer (imports vs transients, non-obvious constraints such as SceneDepth attachment → Read → attachment), plus a link to the goldens and the viewer.
* **Done.** **T3 [med] Migration-log sentences.** :72, 81, 94, 114 each say "The X renderer runs on the graph." Action: delete.
* **Done.** **T4 [med] Implementation detail in a user doc.** :92 and :112 describe Metal compaction commits and `IBLPass::Finalize` command buffers. Action: move to code comments or cut.
* **Done.** **T5 [med] Missing workflows.** Nothing tells the reader how to get a dump from the CLI (`dev/run.py … --test_case render_graph_dump` → `screenshots/render_graph.json`) or how to update a golden. Updating takes two steps: `run_tests.py --case <p>_graph_shape`, then `graph_shape_test.py --golden <p> --update`; `run_tests.py` does not forward `--update`. Action: add both to a Debugging section.
* **Done.** **T6 [low] Inverted knob wording.** :47 says "`render_graph_cull` (default `true`) turns culling off"; setting it to `false` does. The knob is also missing from the docs/Run.md config table, which lists `render_graph_view`. Action: fix the wording and list both knobs in one table.
* **Done.** **T7 [low] Overstated claim.** :128 says the dump is identical at any resolution. `physical` can differ: at scale 1, Scene and Output transients share an extent and may alias. Action: qualify the claim.
* **Done.** **T8 [low] Restated code list.** :70 repeats the format list in `SamplesAsFloat` (`Renderer.cpp:263`). Action: name the function instead.
* **Done** (documented; the rewrite goes at the next NRD recook, a TODO entry). **T9 [med] Stale justification.** docs/Denoiser.md:80: the NRD SPIR-V 1.5 → 1.4 downgrade and the `vulkan1.1spv1.4` validation target (`cook_nrd_shaders.py:114`) no longer have a reason under Vulkan 1.3. Action: remove the rewrite (needs a recook) or state the real constraint; at minimum add a TODO entry.
* **Done.** **T10 [med] Open TODO for finished work.** docs/TODO.md:25 still lists `* [ ] render graph`. Action: narrow it to Phases 3–4.
* **Done.** **T11 [med] Incomplete CI doc.** docs/CI.md:89 says the sync cases gate forward, deferred and gpu, but `cpu_sync_validation` exists too; the loader-manifest detail belongs in code. Action: fix the list and cut to one sentence.
* **Done.** **T12 [low] Test.md drift.** :144 hard-codes case arguments that live in registry.json. :68's evaluator list omits `graph_shape_test.py`. Action: link to the registry; add the evaluator.

Proposed docs/RenderGraph.md outline:

1. Overview (3–4 sentences: what the graph derives; one frame; the RHI only lowers it).
2. Adding a pass to a renderer: an example inside `Render()` → `ExecuteGraph`. The standalone-graph sample moves to Tests.
3. Reference, as bullets: resources (transient, import, subresource, handles), accesses, bindings, pass kinds table, pass flags.
4. What the compiler decides: culling, aliasing, barriers, load/store; one bullet each, pointing to the dump's `*_reason` fields.
5. Errors: the aborting checks as a list (they abort in Release too).
6. Debugging: knobs table (`render_graph_view`, `render_graph_cull`, `validate_sync`), the live UI page, getting a dump, the viewer, pass timing and its latency.
7. Renderers: one paragraph each plus a link to the goldens.
8. Tests: case names and the golden-update workflow.

## 7. Tests, tooling and CI

Coverage of the compiler is better than expected. `render_graph_compile` asserts exact plans for culling, sharing within and across frames, pool release, load/store with reasons, per-subresource barriers, tracked-state seeding, the External contract and buffer barriers, and reads pixels back.

* **Done** (`RGErrorsThrow` makes `RGCheck` throw on its thread; `render_graph_errors` covers 29 mistakes). **T13 [high] No test for any aborting check.** About 31 `RGCheck`s have no negative test; binding validation is the most intricate. Action: route `RGCheck` through a swappable handler (record instead of abort under test), then add a `render_graph_errors` case with one mistake per error class and its expected message.
* **Done** (the acceleration-structure step runs only with hardware ray tracing). **T14 [med] Untested paths.** A merged run of several mips (the 2-mip cube always differs per mip), `SampledOrPlaceholder`, `NativeAccess` inside graph rendering, AS build → read and build → build barriers (`BuildTLAS` is absent from gpu.txt), a buffer imported twice, and `render_graph_view` on renderers other than deferred. Action: add them to `render_graph_compile` where synthetic graphs can reach them.
* **Done** (`render_graph_compile` and `render_graph_errors` also run on windows-glfw, macos-glfw and macos-ios). **T15 [med] Lost platform coverage.** `render_graph_compile` runs only on macos-macos (where barriers record nothing) and, as `render_graph_sync_validation`, on ubuntu. The removed `render_target_pool` ran on 5 triplets. Action: run `render_graph_compile` on every triplet `render_target_pool` covered.
* **Done.** **T16 [med] Golden churn is undocumented.** First-barrier sources depend on the previous frame's graph, pool order, scene content and `shadow_map_resolution`. That is acceptable for a shape gate. Action: document what churns the goldens and the update workflow (T5).
* **Done** (the case dumps again until the frame traces with no clear or TLAS build). **T17 [med, P] GPU golden depends on timing.** `render_graph_dump_accumulating` dumps "as soon as the scene is loaded" and assumes that frame has neither `ClearAccumulator` nor `BuildTLAS`. Action: dump after N traced frames, or on an explicit predicate (accumulating, spp ≥ 1, nothing pending).
* **Done.** **T18 [med, C] Shape cases break under a scene override.** The six `*_graph_shape` registry cases carry `scene_args`, but the goldens are TestScene-specific, so `run_tests.py --scene X` fails all of them. Action: drop `scene_args` from those cases.
* **Closed (owner: keep 0.1).** **T19 [med] 64 spp FLIP gates are loose.** `cpu_render_static_64spp` and `gpu_render_static_64spp` use `--flip_threshold 0.1`, 5× the default. The deferred-shadow defect (FLIP 0.0665) would have passed. The output is deterministic per platform. Action: pin a per-backend 64 spp reference with a tight threshold, or calibrate just above the measured value.
* **Done.** **T20 [low] Viewer page collision.** `graph_shape_test.py:86` names the page after `--golden`, so `deferred_graph_view_fallback` overwrites `deferred_graph_shape`'s page. Action: add a `--page` argument, or name the page per case.
* **Done.** **T21 [low] Typo in `--golden` is a traceback.** Action: validate in `tests/build_system/test_registry.py`.
* **Done** (an evaluator requires `effective pipeline: Gpu` in the app log). **T22 [low] Sync gate can silently test the fallback.** `gpu_sync_validation` passes on a device that falls back to forward. Action: assert the `effective pipeline: Gpu` marker.
* **T23 [low] Duplicated test helpers.** `Expect()` exists in 6 test files (3 new). `PassTimestampTest` and `RenderGraphPassTimingTest` share the same recording/finish state machine. Action: move `Expect` into `TestCase`.
* **Done** (the C++ `Summarize` in `RenderGraphCompileTest.cpp` stays a separate copy). **T24 [med] Dump formatting written three times.** Resource/subresource, barrier and attachment strings are built in `dev/render_graph_viewer.py:42,60-71`, `graph_shape_test.py:27-53` and `RenderGraphCompileTest.cpp:84-140`, with different separators. `project()`, the actual gate, has no unit test. Action: have `graph_shape_test.py` import the viewer's `describe_*` helpers, and unit-test `project()`.
* **T25 [low] Brittle viewer tests.** `test_render_graph_viewer.py:77-114` compares exact HTML strings against a hand-written fixture. Action: assert on key substrings or parsed cells, and generate the fixture from a real dump.
* **Done** (the dump names lifetimes by pass index). **T26 [low] Lifetimes keyed by pass name.** `render_graph_viewer.py:103-109` maps lifetimes by name, but names repeat (`ClearIbl*Cook`), so lifetimes can map to the wrong pass. The viewer also labels only a resource's first attachment (:56). Action: dump pass indices for `first_use`/`last_use` (`RenderGraphDump.cpp:210,227`).
* **Done.** **T27 [low] Inconsistent imports.** `graph_shape_test.py` uses `sys.path` inserts and the viewer test uses `importlib`. Action: pick one.
* **T28 [med] CI restores the whole SDK cache for one layer.** `ci.yml:894-900` restores the full Linux SDK cache (several GB) to get one `.so` and one manifest. A `restore-keys` prefix hit can also restore a mismatched SDK. On a cache miss, `ci.yml:912-914` fails with a traceback instead of a clear message. Action: move the Linux manifest rewrite into `build_system/prerequisites.py` `set_vulkan_layer_path`, which fixes local Linux runs too (see T12). Then either upload the layer as a small build artifact or use `fail-on-cache-miss: true`.
* **T29 [med] CI cost unmeasured.** Every ubuntu case now runs under core validation, plus 5 sync, 6 shape and two 1280×720 64 spp lavapipe runs. Action: record the ubuntu job duration before and after in the PR.
* **T30 [low] Unrelated edits.** `CMakeLists.txt` warning opt-outs (`a46be45`) and the `PickPhysicalDevice` TODO entry (docs/TODO.md:28) are unrelated to the render graph. Action: mention them in the PR, or ship them with the Phase 0 PR (A4).

## 8. Suggested order of action

1. **Decisions for the owner:** settled, see [Owner decisions](#owner-decisions).
2. **Correctness fixes:**
   * M1 (Metal timer) plus a test that rejects 0 ms.
   * G1 (one discard rule), R1 (screenshot fallback), R4 (null guard), M2/R2 (MetalFX fallback).
   * V1/G5 (reset state on a dropped frame), G2 (2D-array storage view), V2 (Present source stage).
3. **Rules the graph must enforce:** R7/V9/M9/G11 (record only through pass contexts), G4 (contract check for every pass kind), V5 (asserts), V12 (labels on every pass).
4. **Trim:** M6/V7 (speculative capabilities), G11 (dead API), V8/R8 (leftovers), G13 (internals out of the header), A2 (`DontCare` naming), G15–G19/V14 (DRY).
5. **Tests:** T13 (negative tests for the aborts), T15 (triplet coverage), T17 (GPU golden predicate), T18 (`scene_args`), T19 (64 spp thresholds), T14 (untested paths).
6. **Docs:** T1–T12 (restructure docs/RenderGraph.md per the outline), A5 (design header), and G9/R3 (record the deviations in the design).
