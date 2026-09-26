# Render Graph Research Review

Survey of production and independent render graph designs, plus the platform facts that constrain a Vulkan + Metal implementation, read against Sparkle's goals: make resource dependencies explicit and visualizable, keep data on-chip (merged passes, memoryless attachments), and enable multi-threaded recording. The companion design is [RenderGraphDesign.md](RenderGraphDesign.md). Raw notes with a citation for every claim live in [render_graph_notes/](render_graph_notes/): [commercial.md](render_graph_notes/commercial.md), [independent.md](render_graph_notes/independent.md), [platform.md](render_graph_notes/platform.md), plus the two code maps [code_rhi.md](render_graph_notes/code_rhi.md) and [code_renderer.md](render_graph_notes/code_renderer.md).

Sources: public source code was read directly for Unity SRP, Godot, O3DE, AMD RPS, Granite, Filament, kajiya, vuk, Daxa, Falcor, skaarj1989/FrameGraph and daFrameGraph (default branches, 2026-09-25). Unreal internals come from official docs, the API reference and cvar descriptions (the source is under EULA). Driver support comes from live gpuinfo.org queries. Items marked *unverified* had no primary source.

## 1. Taxonomy

Every system surveyed falls into one of five shapes. The shape matters more than the feature list, because it decides what the graph can know.

| Shape | Examples | What it knows | What it cannot do |
| --- | --- | --- | --- |
| Declarative per-frame graph (setup → compile → execute) | Frostbite FrameGraph, Unreal RDG, Unity RenderGraph, Filament, Granite, kajiya | Whole-frame resource lifetimes and every pass's accesses | Nothing structural; cost is authoring discipline |
| Compile-once graph with cached permutations | Daxa TaskGraph, Activision task graph, AMD RPS | Same, plus schedule computed offline | Cheap structural change per frame (needs recompiles or permutation keys) |
| Implicit graph inside the RHI | Godot 4.3 RenderingDeviceGraph | Per-command resource usage | Culling, aliasing, subpass/merge decisions (no whole-frame intent) |
| Execution-order / module graph | Bevy ≤0.18, Falcor | Pass order and named I/O edges | Barriers, lifetimes, load/store (resources not declared) |
| No graph, convention or tracking only | HypeHype, Blade, Wicked, The Forge, Diligent | Nothing global | Anything whole-frame; relies on conventions |

Sparkle today is the last shape with a hand-written convention per renderer (see §7). The goals (lifetimes, on-chip merging, visualization) need the first shape; the other shapes contribute individual techniques.

## 2. Production systems

### 2.1 Unreal RDG

* Model: `FRDGBuilder::AddPass(name, params, flags, lambda)`. The pass parameter struct (`BEGIN_SHADER_PARAMETER_STRUCT`) is both the dependency declaration and the shader binding. Resources are virtual handles until compile; `GetRHI()` outside a lambda asserts. Rebuilt every frame.
* Compile: cull from roots (extracted/external writes, `NeverCull`), per-subresource state tracking, split barriers batched into per-pass prologue/epilogue, transient heap aliasing (`r.RDG.TransientAllocator`), render-pass merging of **identical contiguous render targets only** (`r.RDG.MergeRenderPasses`).
* Subpasses are not inferred. Mobile deferred is hand-authored inside one RDG pass with `ESubpassHint` and explicit `NextSubpass()`, GBuffer targets created memoryless.
* Parallelism: contiguous spans of parallel-safe passes (`r.RDG.ParallelExecute.PassMin/Max`) record into separate command lists, submitted in original order. **Parallel setup and parallel execute are disabled on mobile platforms.** Async compute is a manual per-pass flag; RDG only computes fork/join fences and extends lifetimes.
* Tooling worth copying: every optimization has a kill-switch cvar (cull, merge, parallel, transient, async) for bisection; immediate mode (`r.RDG.ImmediateMode`) restores call stacks; RDG Insights shows lifetimes, merges, parallel ranges and transient layouts.
* Tech debt visible in the API: `ClearUnusedGraphResources` (unused shader params create false dependencies because declaration = binding), `SkipTracking`, `UseExternalAccessMode`, `SkipRenderPass`, `NeverParallel` — each exists to interoperate with code the graph cannot see.

### 2.2 Unity RenderGraph (URP 17 / Unity 6)

* Model: `AddRasterRenderPass / AddComputePass / AddUnsafePass` with a builder (`UseTexture`, `SetRenderAttachment(tex, index)`, `SetRenderAttachmentDepth`, `SetInputAttachment`, `AllowPassCulling`) and a static render function.
* Why the pass types split: the old generic pass exposed `SetRenderTarget` on a free-form command buffer, so the compiler could not know a pass's attachments and could not emit native render passes (source comment: the old API "can't express detailed frame information needed to emit native render passes"). A raster pass gets an encoder that cannot change targets. This is the single most transferable lesson for Sparkle, whose passes currently own their render passes.
* NativePassCompiler (read from source): validate → build → cull → **greedy merge in submission order (no reordering)** → usage ranges and sync → memoryless detection → prepare native passes. Consecutive raster passes merge automatically when attachments are compatible; a pass that *samples* a texture written inside the open native pass breaks it (`NextPassReadsTexture`), while reading it through `SetInputAttachment` keeps it on-chip. Passes with identical attachments share one native subpass ("nextSubpass is expensive on some platforms").
* Every decision carries a reason: `PassBreakReason` (TargetSizeMismatch, NextPassReadsTexture, NonRasterPass, DifferentDepthTextures, AttachmentLimitReached, SubPassLimitReached, …) and load/store audits (LoadPreviouslyWritten, ClearCreated, StoreUsedByLaterPass, DiscardUnused, …). The Render Graph Viewer shows a pass × resource grid, merged native passes, the break reason for each boundary, load/store with reasons, and per-resource memoryless status.
* Memoryless rule: a non-imported texture whose whole lifetime is inside one native pass becomes memoryless. Pooling is a descriptor-hash texture pool with intra-frame reuse, no heap aliasing.
* Compile caching by FNV hash of the declared graph. No parallel recording in the graph (native graphics jobs below it).
* Costs: boilerplate complaints, global shader state had to be retrofitted (`AllowGlobalStateModification`, which disables culling), greedy merge means one compute pass in the wrong spot splits a native pass, and keeping the old path alive took ~3 releases (2023.3 → 6.4).

### 2.3 Godot 4.3+ RenderingDeviceGraph

* The graph lives inside `RenderingDevice` and is invisible to callers. Each resource has a tracker (last writer, reader list); a layout change counts as a write. Draw lists and compute lists are single coarse nodes (~300 per frame).
* Topological levels of independent commands, grouped by type, one barrier batch per level: 60–80% fewer barrier calls, 5–15% frame time improvement, ~2,500 lines of manual sync deleted, graph cost under 1% CPU.
* 4.4 inferred load/store actions: a Mali-G715 MSAA test went from 52 to 120 FPS purely from discarding MSAA writes.
* Secondary command buffers exist but are disabled (`SECONDARY_COMMAND_BUFFERS_PER_FRAME 0`) after driver issues including NVIDIA crashes. Subpasses on the Mobile renderer are explicit multipass framebuffers.
* Limits of the shape: no culling, no aliasing, no merge inference, lost backtraces.

### 2.4 Frostbite FrameGraph (GDC 2017)

* Origin of the setup-lambda / execute-lambda pair. Setup declares `read/write/create`; compile culls by refcount, computes lifetimes, derives bind flags from usage, allocates transients just in time; execute runs callbacks.
* Motivation was modularity as much as optimization: WorldRenderer shrank from ~15k to ~5k SLOC, conditional features became plain `if`s, disconnecting a debug output auto-disables everything upstream.
* Transient aliasing: 147 MB → 80 MB at 720p (DX12 PC), 1042 → 472 MB at 4K. Async compute is a one-line per-pass flag; placement stays manual.
* Blackboard for passing handles between modules; persistent modules own history.

### 2.5 Others

* **Activision task graph (REAC 2023)** — the best industrial retrospective. Macro DSL generates the access table, handle struct and setup function from one definition. Heavy compile at level load, then per-frame cached permutations keyed by a 5–10-bit condition mask. First-class temporal resources (auto-rotated, nulled on camera cut) and null resources for optional features. ~6000 → ~1500 lines of glue, 25% less CPU render time, 100–500 MB saved. On mobile the scheduler prefers placing render-pass-compatible tasks adjacently. Draw-list splitting across command buffers "wasn't viable on TBDR hardware"; parallelism became task batches recorded in GPU order with hand-off back to the render thread.
* **O3DE Atom** — separate *compile-resources* phase between allocation and recording, so recording threads see finished descriptor sets. Stage/access/layout deduced from a usage enum. Subpasses opt-in (`MergeChildrenAsSubpasses`); its RFC documents the Vulkan trap that PSOs built against one VkRenderPass only work in *compatible* ones, including identical subpass dependency declarations.
* **EA SEED Halcyon** — generational 64-bit render handles, stateless high-level command lists that are parallel-recording friendly, fully automatic transitions and split barriers. Honest caveats: ~5% lost to aliasing barriers and discards on PC; automatic queue scheduling "ongoing research … not enough to specify dependencies".
* **AMD RPS** — HLSL-extended graph language with access attributes on node signatures, full scheduler with policies, range recording for multi-threading. No Metal backend; last commit May 2024.

## 3. Independent systems

* **Granite** (Themaister) — the most complete independent answer for tilers. String-keyed resources; bottom-up traversal from the backbuffer; greedy reorder that the code itself calls "very inefficient". Automatic subpass merging via ~130 lines of `should_merge` rules: refuse across queues or compute, refuse if the next pass samples (non-locally) anything the previous wrote, refuse on different depth; accept only if merging keeps data on tile (color feedback, shared depth, input attachment). Transient = image whose readers and writers all sit in one physical pass → lazily allocated. Aliasing only for same-size same-format images. History via image swap. Multi-threading in two timelines: a serial CPU pass resolves barrier state, then one command buffer per physical pass recorded in parallel, submitted in order; no secondaries. ~4,970 lines.
* **Filament FrameGraph** — cleanest production design for a small team. Typed versioned handles (`read()`/`write()` return a new handle), declaration order, ~40-line refcount cull, usage flags as the union of accesses, **load/store derived from edges** (no prior writer → discard start, no later reader → discard end, nobody writes depth → read-only). Emits no barriers itself (backend tracks layouts). Subpasses explicit and capped at 2. Graphviz dump and `fgviewer` web app. ~4,900 lines.
* **kajiya** (archived) — just-in-time barriers from the last access type; declaring an access in `SimpleRenderPass` also pushes the shader binding in order, so nothing is written twice. Best temporal API seen: `get_or_create_temporal(key, desc)` imports with the last known access state and exports at frame end.
* **vuk** — SSA IR over futures with extent/format/queue inference; elegant API, compiler-sized implementation. The README's automatic subpass deduction is not in the current backend (one subpass per pass, "we use barriers").
* **Daxa TaskGraph** — compile once, ASAP batches with one barrier set per batch, no subresource tracking; docs admit reordering conflicts with aliasing. ~9,700 lines with UI.
* **Falcor** — Python-scripted plugin graph with a live node editor; strong as a *viewer and wiring* tool, does nothing for performance.
* **Bevy** — its execution-only graph (no resource declarations) was **deleted in 0.19** in favor of ECS schedules. A graph that does not know resources adds nothing over a scheduler.
* **Small references** — skaarj1989/FrameGraph (~1,550 lines, policy in resource-type hooks, dot + web viewer), Liam Tyler's "poor man's render graph" (no reorder, no prune, one queue — "you absolutely shouldn't start with any of those things"), Deep Spark MVP series (~90 lines scaffold, ~260 with barriers, ~500 with aliasing), Our Machinery (sort keys decouple CPU recording order from GPU order), daFrameGraph (`historyFor` reads create no edges).
* **Sandu & Shcherbakov, WSCG 2024** — frames optimal render-pass merging as NP-complete; Granite-style automatic merging was "less optimal than the manual approach" on a shipping mobile title. Argues for intent-driven merging over search.

## 4. The case against heavy graphs

* **Aaltonen, "No Graphics API" (Dec 2025)** — barriers should be producer/consumer stage masks with no resource list; layout transitions are obsolete on current hardware; Vulkan subpasses were a misstep that exists "simply to avoid exposing the framebuffer fetch intrinsic"; render passes with load/store ops should stay. HypeHype ships a convention instead of a graph: every texture stays in sampled layout except inside its own render pass.
* **Malyshau, "Global Pass Barriers Without Per-Resource RHI Tracking" (arXiv 2607.26506, July 2026)** — Blade keeps images in GENERAL and emits global barriers derived from the *kinds* of passes around each boundary; saves 5–7% on graphics/compute chains vs redundant barriers, and wgpu's per-resource tracking costs 1.3–5.9× the host time. The paper explicitly excludes tilers, where "a barrier that breaks a render pass … forces a tile flush and reload".
* **Jotun "rebuttal of render graphs" (2021)** — you declare everything twice and dependencies stay implicit. Both are answered by reusing one access list for barriers, load/store, usage flags and binding validation, and by typed handles that passes return.

Takeaway: barrier derivation is the *least* valuable thing a graph does for a small renderer, and APIs keep making it cheaper. The things only a whole-frame view can do are lifetimes (transient, memoryless, aliasing), correct load/store, culling, and keeping tiler render passes unbroken — exactly Sparkle's goals.

## 5. Platform facts that constrain the design

Full matrix and citations in [platform.md](render_graph_notes/platform.md) §7–8.

* **On-chip merging primitive.** Arm, Qualcomm and Khronos all point tilers at dynamic rendering + `VK_KHR_dynamic_rendering_local_read`. Arm (Nov 2025) ranks it above rasterization-order access, `shader_tile_image` and classic subpasses, and calls subpass merging implementation-dependent; plain dynamic rendering *disables* subpass fusion on Mali without local_read. `VK_QCOM_tile_shading` builds on local_read with "equivalent" performance expected for render pass objects and dynamic rendering. **MoltenVK turns every Vulkan subpass into a separate Metal render pass** (issue #2454), so subpasses are actively harmful on the glfw/macOS configuration.
* **Support.** S25 Ultra (Adreno 830, Samsung 512.800.x) has local_read, lazily allocated memory, rasterization-order access and tile_properties; lacks unified_image_layouts, tile_shading, tile_memory_heap and nested command buffers. Mali from r49/r50 on G710+. NVIDIA, AMD and MoltenVK 1.4.0+ have it. Android 16 launch devices must support Vulkan 1.4, which guarantees color (not depth/MSAA) local read. Sparkle's Android `minSdk` is 31, so older devices are not guaranteed any of this.
* **Metal.** One render encoder = one render pass. Programmable blending (`[[color(n)]]`) on all Apple GPU families, **not** on Intel/AMD Macs, and **color only — no depth fetch**. No fragment→fragment barrier inside a pass on Apple GPUs; same-pixel ordering is implicit. So both APIs share one rule: only same-pixel reads may merge passes. Slang lowers `SubpassInput` + `input_attachment_index(N)` to `[[color(N)]]` (reported in Slang 2026.9, *unverified*; Sparkle pins 2026.12.2).
* **Transient memory.** Vulkan TRANSIENT images may carry only color/depth/input-attachment usage (never sampled, storage or copy). Lazily allocated memory exists on Adreno, Mali and MoltenVK (mapped to memoryless), **not on NVIDIA/AMD desktop**. `MTLStorageModeMemoryless` lives only within one render encoder. Arm measured ~600 MiB/s saved per attachment by CLEAR instead of LOAD and ~555 MiB/s by DONT_CARE instead of STORE on depth; inline 4× MSAA resolve at 1080p60 costs 500 MB/s vs 3.9 GB/s with a separate resolve.
* **Barriers.** sync2 is universal on targets. On Mali, fragment→vertex/compute (backward) dependencies create bubbles; COLOR_ATTACHMENT_OUTPUT→FRAGMENT instead of →VERTEX cut frame time 13%. `VK_KHR_unified_image_layouts` helps desktop only (not AMD Windows, not Mali G7xx/G9xx, not MoltenVK, Adreno only from 512.849); GENERAL is a transaction-elimination-unsafe layout on Mali (~10% more write bandwidth). Precise layouts still pay off on mobile.
* **Metal 3 vs 4.** Metal 3 tracks hazards automatically for device-created resources (heap resources default untracked). Metal 4 tracks nothing: the graph must emit stage barriers, residency sets and lifetimes. Sparkle's deployment targets (macOS 14.2, iOS 18.0) predate Metal 4, so Metal 3 tracked resources remain valid for now.
* **Multi-threaded recording.** Across passes: parallel primary command buffers submitted in order (Vulkan), `enqueue()`-ordered parallel `MTLCommandBuffer`s (Metal 3), `commit:count:` arrays (Metal 4). Within one render pass: Vulkan secondaries or dynamic-rendering suspend/resume (same batch, nothing in between); Metal 3 `MTLParallelRenderCommandEncoder`; Metal 4 suspend/resume with the same structural rule as Vulkan. Secondaries are acceptable on Adreno ≥650 and Mali ≥G710 in moderate counts; no public data compares suspend/resume vs secondaries on tilers.
* **Async compute.** Adreno LPAC is low-priority, suited to multi-millisecond latency-tolerant work; Mali gains are small (~5%); Apple single queues already overlap. Not worth it initially.

## 6. Cross-cutting findings

| Concern | Converged practice | Outliers and why |
| --- | --- | --- |
| Rebuild cadence | Every frame, from code; conditional passes are `if`s | Daxa/Activision/RPS compile once for scale (100s of passes); Unity caches by hash |
| Dependency declaration | Builder calls in a setup phase, separate from binding | UE fuses with shader params (false deps); kajiya/Daxa fuse with binding order (no duplication) |
| Ordering | Declaration order, cull by refcount | Granite/Daxa reorder (complexity, fights aliasing); Godot levels (inside RHI) |
| Barriers | Derived from a closed usage enum, batched per boundary | Filament leaves them to backend tracking; Blade/Aaltonen argue for global stage barriers |
| On-chip merging | Unity: automatic by rules, pixel-local reads declared as input attachments, reasons recorded | UE/O3DE/Godot/Filament: hand-authored; Granite: automatic with reorder |
| Load/store | Inferred from graph edges (Unity, Filament, Godot 4.4) | Hand-written anywhere older |
| Transient memory | Descriptor-keyed pool + memoryless for pass-local attachments | Heap aliasing where memory is tight (UE, Frostbite, Activision) |
| History | First-class keyed temporal resources (kajiya, Activision, Granite, Daxa) | UE extract/re-import dance; Filament imports from outside |
| MT recording | Contiguous spans → one command buffer each, submitted in order | Secondaries disabled in Godot, deemed not viable on TBDR by Activision, parallel execute off on mobile in UE |
| Async compute | Manual per-pass flag, graph computes fences | Automatic placement is "ongoing research" everywhere |
| Tooling | Dump (dot/JSON) + per-decision reasons + kill switches | Editors (Falcor) are nice-to-have |

## 7. How Sparkle compares today

Condensed from the code maps; details and file:line references in [code_rhi.md](render_graph_notes/code_rhi.md) and [code_renderer.md](render_graph_notes/code_renderer.md).

* **Frame structure.** Four renderers (Forward, Deferred, GPU path tracer, CPU) each run 5–10 passes per frame into one primary command buffer on the render thread. Every `PipelinePass` owns its own `RHIRenderTarget` and `RHIRenderPass` with hard-coded load/store and layouts; no two passes share a render pass. Wiring is constructor arguments plus setters that rebind descriptors (`SetInput`, `SetIBL`, `OverrideSkyMap`), and camera-proxy replacement has to be detected by hand — the stale-camera-proxy bug class.
* **Barriers.** Renderers call `RHIImage::Transition` by hand with hand-picked stages, duplicated across all four renderers. Vulkan skips the barrier whenever old and new layout are equal, so StorageWrite→StorageWrite and General↔StorageWrite (all GENERAL) emit nothing: storage RAW/WAW hazards are unsynchronized. Buffers have no state tracking. There is no acceleration-structure-build → ray-query barrier and `RHIPipelineStage` cannot express one. Render-pass `final_layout` overwrites the tracked layout while `color_initial_layout` is never reconciled with it (two sources of truth). Metal relies on automatic hazard tracking; `Transition` is a no-op.
* **Render passes.** Vulkan API 1.1 with `VkRenderPass`/`VkFramebuffer`, one subpass, framebuffers created at pass creation and tied to image views; PSOs baked against a specific render pass with viewport baked from the target extent. One load/store op for all color attachments. `NextSubpass` is `UnImplemented` on both backends. No transient/memoryless mapping (`TransientAttachment` usage is dropped on Vulkan and only adds RenderTarget usage on Metal).
* **Allocation.** `RHIRenderTargetPool` is an exact-attribute cache that survives renderer recreation; renderers hold targets for their whole lifetime. Any resolution or render_scale change recreates the renderer and scene proxies.
* **Hidden GPU work.** IBL cook dispatches and TLAS builds are recorded during `Tick`; uploads, `PartialUpdate`, readbacks and a Metal mid-frame TLAS commit-and-wait record or submit wherever they are called; NRD, MetalFX and ImGui record opaque commands (MetalFX directly on the `MTLCommandBuffer`, ImGui raw `vkCmd*` that leaves the tracked bind state stale).
* **Threading and timing.** Single-threaded recording enforced by a global backend `context`, a single command pool and implicit current-pass state. Only compute passes with `need_timestamp` are GPU-timed; raster passes have no timing on either backend.

Seams that already fit a graph: the Tick/Render split (setup before record), exclusive non-nested pass brackets in `RHIContext`, per-subresource layout tracking in `RHIImage`, stable `RHIResource::GetId()`, frames-in-flight-delayed deletion, the render-target pool, `RenderResolution` as the size-class source, the provider-neutral `Denoiser` boundary, `PassTimingAggregator`, and the screenshot readback hook.

## 8. Lessons for Sparkle

Copy:

1. Setup + record callback per pass, rebuilt every frame; record callbacks side-effect-free so they can move to worker threads later (UE's rule).
2. Typed pass kinds whose encoder cannot change attachments (Unity). The graph, not the pass, begins and ends render passes. One explicit escape hatch for foreign code.
3. Separate declarations for attachments, sampled reads and pixel-local reads. The pixel-local read is the only authoring intent merging needs (Unity input attachments, Granite, platform rule C1).
4. Load/store and memoryless inferred, never hand-written, and every decision audited with a reason (Unity, Filament, Godot 4.4).
5. Barriers derived from a closed usage enum, batched per boundary, precise stages (Godot, O3DE, Arm guidance).
6. Coarse pass-level nodes; declaration order; refcount culling (Filament, Godot).
7. Keyed persistent/history resources imported with their last state (kajiya, Activision).
8. Parallel recording by physical-pass spans into separate primaries, submitted in order; split *within* a pass only for measured heavy passes (UE, Granite, Activision).
9. Kill-switch per optimization and a serial debug mode (UE); a machine-readable dump before any UI (Filament, skaarj).

Avoid:

1. Fusing dependency declaration with shader-parameter reflection (UE false dependencies). Use the reflected bindings only to *validate* declarations.
2. Implicit RHI-level graph as the only graph (Godot): no culling, aliasing or merge decisions.
3. Pass reordering and scheduling heuristics at our pass counts (Granite, Daxa); NP-hard in general (Sandu).
4. Hand-authored subpass chains (UE `ESubpassHint`, O3DE), and Vulkan subpasses as the core abstraction (MoltenVK splits them, Arm calls them unpredictable, Aaltonen calls them a misstep).
5. Secondary command buffers as the default parallelism path (Godot, Activision, UE-mobile experience).
6. Automatic async compute; heap aliasing before memoryless; compile-once graphs, IRs, scripting and editors.
7. A long-lived dual path (Unity's three releases); port renderers fully instead.
