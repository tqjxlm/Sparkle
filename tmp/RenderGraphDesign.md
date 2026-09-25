# Render Graph Design (proposal)

Proposal for a lean render graph in Sparkle. Nothing here is implemented. Research behind every choice: [RenderGraphResearch.md](RenderGraphResearch.md); raw notes and code maps with file:line references: [render_graph_notes/](render_graph_notes/). Decisions still open are collected in §14 with pro/con/suggestion; the rest of the doc assumes the suggested answer so the design reads end to end.

## 1. Goals and non-goals

Goals, in priority order:

1. **Explicit dependencies.** Every GPU pass declares what it reads and writes. The frame becomes a data structure that can be dumped, diffed, visualized and gated in tests.
2. **Derived synchronization.** Barriers, image layouts, load/store actions and resource usage flags are computed from the declarations. No renderer code calls `Transition` or picks a load op.
3. **On-chip data.** Consecutive raster passes that communicate only through same-pixel reads merge into one hardware render pass; attachments that never leave that pass become memoryless / lazily allocated. The graph reports *why* any two adjacent passes were not merged.
4. **Multi-tasking readiness.** Pass recording is side-effect free against an explicit command context, so physical passes can be recorded on worker threads and submitted in order once profiling justifies it. The dump shows which passes are independent.

Non-goals for the first version: pass reordering, async compute, heap memory aliasing, Metal 4, compile-once graphs, scripting or node editors. Each has a hook left open (§14) but no code.

Size target: ~1,000–1,500 lines for graph core + compiler + dump, plus backend lowering. Reference points: Deep Spark MVP ~500, skaarj1989 ~1,550, Filament and Granite ~5,000 (the ceiling to stay well below).

## 2. How the frame works today

Details in [code_renderer.md](render_graph_notes/code_renderer.md) and [code_rhi.md](render_graph_notes/code_rhi.md).

* **Threads.** The main thread ticks the scene and pushes proxy updates as closures to the render thread (at most one frame ahead, `MaxBufferedTaskFrames = 1`). The render thread runs `RenderFramework::RenderLoop`: consume tasks → `RHIContext::BeginFrame` (fence wait, acquire, begin the single primary command buffer / new `MTLCommandBuffer`) → `Renderer::Tick` (proxy updates, UBO uploads, and some GPU work: IBL cook dispatches, TLAS builds) → `Renderer::Render` → `RHIContext::EndFrame` (present transition, submit, present).
* **Renderers.** Forward, Deferred, GPU (compute path tracer with inline ray query, NRD/MetalFX denoisers) and CPU (software path tracer + upload). All share a tail: tonemap → optional readback → optional ImGui → optional readback → present quad into the swapchain.
* **Passes.** `PipelinePass` has `InitRenderResources`, `UpdateFrameData`, `Render`. Each pass builds its own `RHIRenderTarget` + `RHIRenderPass` at construction with hard-coded load/store/layouts and calls `BeginRenderPass/EndRenderPass` itself. Inputs arrive through constructors and setters that rebind descriptors. PSOs are baked against the pass's `VkRenderPass`, with viewport baked from the target extent.
* **Synchronization.** Renderers insert `RHIImage::Transition` calls by hand between passes. Vulkan tracks one layout per subresource and skips barriers whenever the layout does not change (storage RAW/WAW is therefore unsynchronized); buffers and acceleration structures are untracked. Metal relies on automatic hazard tracking.
* **Resources.** Renderers hold render targets for their lifetime (via `RHIRenderTargetPool`, an exact-match cache that survives renderer recreation). Any resolution or `render_scale` change recreates the renderer and scene proxies.

Today's Deferred frame at `render_scale = 1`, per pixel, counting attachment and full-screen texture traffic only (arithmetic estimate, not a measurement): gbuffer stores 16 B + depth 4 B; lighting samples 20 B and writes 8 B; skybox loads and stores scene color 8+8 B and loads depth 4 B; tonemap samples 8 B and writes 4 B; UI loads and stores 4+4 B; present samples 4 B and writes 4 B. About 96 B/pixel, ~88 MB/frame at 1280×720, ~5.3 GB/s at 60 fps. §12 shows the same frame after the graph.

## 3. Principles

1. **Coarse nodes.** A node is a pass (a draw loop or a dispatch sequence), never a draw. A frame has 5–20 nodes, so compile cost is negligible and the graph is rebuilt every frame.
2. **Declaration order is execution order.** No reordering. Dependencies come from "last writer before me" in declaration order, which also removes the need for versioned handles.
3. **One access list per pass is the single source of truth** for barriers, layouts, load/store, creation usage flags, memoryless eligibility and debug validation of shader bindings.
4. **The graph owns render passes.** A raster pass gets a context that can draw but cannot begin, end or change attachments. This is what makes merging and load/store inference possible (Unity's lesson).
5. **Every compiler decision is recorded with a reason**, and every optimization has a kill switch.
6. **Refactor the RHI to fit the graph**, not the other way around. Persistent `RHIRenderPass`/framebuffer objects, PSO↔render-pass coupling and implicit current-pass state go away.
7. **One path.** Every renderer is ported; there is no "legacy mode" switch.

## 4. Core model

### 4.1 Resources

* **Handles.** `RGTexture` and `RGBuffer` are 32-bit indices into the frame's resource table. They are plain values captured by execute lambdas. No versioning: with no reordering, the compiler resolves each access against the previous access in declaration order.
* **Descriptor.** `RGTextureDesc { PixelFormat format; RGExtent extent; uint8_t mips = 1; uint8_t layers = 1; uint8_t samples = 1; bool cube = false; }`. `RGExtent` is a size class resolved at compile time: `Scene` (`RenderResolution::scene`), `Output` (`RenderResolution::output`), or `Absolute{w, h}` (shadow maps, LUTs). Usage flags are not in the descriptor; they are the union of the declared accesses.
* **Kinds.**
  * *Transient*: created by `graph.CreateTexture(name, desc)`, lives within the frame, backed by the transient pool (§6.4) or memoryless storage.
  * *Imported*: `graph.Import(name, RHIResourceRef<RHIImage>)` for anything persistent: path-tracing accumulator, denoiser outputs and history, IBL maps, sky map, TLAS, bindless arrays, dynamic UBOs, the swapchain image, readback staging buffers. The graph reads the image's tracked layout at compile and writes the final layout back after execution, so state flows across frames through the existing `RHIImage` tracker.
  * *Extracted*: `graph.Extract(handle, &ref)` hands a transient's physical image out beyond the frame (rarely needed; see §14 Q12).
* **Culling roots.** Writes to imported resources, `SideEffect()` passes (readback, present), and extracted resources.

### 4.2 Accesses

A closed enum. The pass kind supplies the default shader stage (raster → fragment, compute → compute); `Stage::Vertex` can be OR-ed for vertex-stage reads.

| `RGAccess` | Meaning | Vulkan stage / access / layout | Metal |
| --- | --- | --- | --- |
| `ColorWrite(slot, clear?)` | color attachment, contents written or blended | COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_READ\|WRITE / COLOR_ATTACHMENT_OPTIMAL (RENDERING_LOCAL_READ inside a group that pixel-reads it) | render attachment |
| `DepthWrite(clear?)` | depth attachment, test + write | EARLY\|LATE_FRAGMENT_TESTS / DS_READ\|WRITE / DEPTH_ATTACHMENT_OPTIMAL | depth attachment |
| `DepthTest` | depth attachment, test only | EARLY\|LATE_FRAGMENT_TESTS / DS_READ / DEPTH_READ_ONLY_OPTIMAL | depth attachment, store DontCare unless needed |
| `PixelLocalRead(slot)` | fragment reads the value written at the *same pixel* by an earlier pass | FRAGMENT_SHADER / INPUT_ATTACHMENT_READ / RENDERING_LOCAL_READ | `[[color(slot)]]` framebuffer fetch |
| `Sampled` | shader reads any texel | stage / SHADER_SAMPLED_READ / SHADER_READ_ONLY_OPTIMAL | tracked read |
| `StorageRead`, `StorageWrite`, `StorageReadWrite` | UAV image or storage buffer | stage / SHADER_STORAGE_READ\|WRITE / GENERAL | tracked read/write |
| `CopySrc`, `CopyDst` | transfer | COPY / TRANSFER_READ\|WRITE / TRANSFER_SRC\|DST | blit encoder |
| `Uniform`, `VertexInput`, `IndexInput`, `IndirectArgs` | buffer reads | UNIFORM / VERTEX_INPUT / DRAW_INDIRECT | tracked read |
| `AccelerationStructureBuild`, `AccelerationStructureRead` | TLAS/BLAS | AS_BUILD / AS_WRITE, stage / AS_READ | AS encoder / tracked |
| `Present` | final state of the swapchain image | — / — / PRESENT_SRC | drawable |

`PixelLocalRead(slot)` requires the resource to be bound as `ColorWrite(slot)` earlier in the same physical pass (§6.3). If the compiler cannot keep the two passes together, it lowers the access to `Sampled` and the pass's execute sees `ctx.IsPixelLocal(h) == false` to pick the sampled shader variant (§7.3).

### 4.3 Passes

Four kinds, each with a context type exposing only what the kind may do:

| Kind | Context can | Context cannot |
| --- | --- | --- |
| `Raster` | bind PSOs, set viewport/scissor, draw | begin/end/change render pass, copy, dispatch |
| `Compute` | bind PSOs, dispatch, dispatch indirect | draw, copy |
| `Copy` | copy/blit/clear images and buffers, generate mips, build acceleration structures | draw, dispatch |
| `External` | get raw backend handles (command buffer / encoder state per declared needs) | — (ends any merge, fully barriered on its declared accesses, resets tracked bind state afterwards) |

A `Raster` or `Compute` pass that must call third-party code recording raw commands (ImGui) declares `b.NativeAccess()`: its context then also exposes the native command buffer / encoder, and the tracked bind state is reset after it. Unlike `External`, it keeps its declared attachments and can still merge.

A pass is declared with a setup lambda that receives a builder and returns the execute lambda:

```cpp
graph.AddRasterPass("Lighting", [&](RGBuilder &b) {
    b.PixelLocalRead(gbuf.packed, 0);
    b.PixelLocalRead(gbuf.view_z, 2);
    b.DepthTest(depth);
    b.Sampled(shadow_map);
    auto out = b.ColorWrite(b.CreateTexture("SceneColor", {PixelFormat::RGBAFloat16, RGExtent::Scene}), 1);
    return [this, gbuf, shadow_map, out](RGRasterContext &ctx) {
        BindInputs(ctx, gbuf, shadow_map);   // ctx.Image(h) resolves handles to RHIImage*
        ctx.DrawFullscreen(pso_);
    };
});
```

Execute lambdas run after compile, possibly on another thread later (§9). They may touch only captured values, the pass object's own persistent state (PSOs, shaders) and the context.

### 4.4 Frame lifecycle

```text
Renderer::Tick     host-side data only: proxy updates, UBO writes into dynamic ring, change lists
Renderer::Render   RenderGraph graph(rhi_, resolution_);
                   BuildGraph(graph);        // renderer composes module functions (§5)
                   graph.Compile();          // §6, pure CPU, no RHI calls
                   graph.Execute();          // §7, records into the frame's command context(s)
                   graph.Dump() if requested // §10
```

Uploads that already record copies (`RHIImage::Upload`, staging buffer copies) stay in a frame *prologue* that runs before the graph and ends with one transfer→all-stages barrier, as today. Moving them into Copy passes is optional (§14 Q11). GPU work currently recorded in `Tick` (IBL cook, TLAS build) moves into graph passes (§8).

## 5. Authoring model

Renderers become compositions of free *module functions* that take handles and return handles (Filament/Frostbite style). Persistent state stays in the existing pass classes, which lose their render targets and render passes and keep their PSOs and shaders.

```cpp
// shared by Forward, Deferred, GPU, CPU renderers
RGTexture AddPostChain(RenderGraph &g, PostPasses &p, RGTexture scene_color, const FrameFlags &f)
{
    auto screen = p.tone_mapping->AddTo(g, scene_color);             // ColorWrite(slot 3) @ Output
    if (f.readback == Readback::WithoutUi) AddReadback(g, screen, f.readback_target);
    if (f.render_ui) p.ui->AddTo(g, screen);                          // Raster pass with native access (§8)
    if (f.readback == Readback::WithUi) AddReadback(g, screen, f.readback_target);
    p.present->AddTo(g, screen, g.ImportBackBuffer());
    return screen;
}

void DeferredRenderer::BuildGraph(RenderGraph &g)
{
    auto shadow = directional_shadow_pass_ ? directional_shadow_pass_->AddTo(g) : RGTexture{};
    auto [gbuf, depth] = gbuffer_pass_->AddTo(g);
    auto scene_color = directional_lighting_pass_->AddTo(g, gbuf, depth, shadow, ImportIbl(g));
    if (sky_box_pass_) sky_box_pass_->AddTo(g, scene_color, depth);
    AddPostChain(g, post_, scene_color, frame_flags_);
}
```

This removes the per-renderer duplication of the post chain, readback and barrier code, and replaces setter-based rewiring (`SetInput`, `SetIBL`, `OverrideSkyMap`, camera-proxy change detection) with values passed each frame. The `RenderConfig::output_image` debug switch generalizes to "show any graph resource by name" (§10).

## 6. Compiler

Input: passes with access lists in declaration order. Output: an execution plan (physical passes, per-boundary barrier batches, resolved physical resources, load/store per attachment) plus the audit data for the dump. All steps are linear or near-linear in passes × accesses.

### 6.1 Validation

* Every handle used by an access exists; transients are written before they are read (otherwise error, not UB).
* A pass does not declare conflicting accesses to one subresource (e.g. `Sampled` and `ColorWrite` of the same mip).
* `PixelLocalRead(slot)` has a matching earlier `ColorWrite(slot)` of the same resource.
* Attachment extents and sample counts within a raster pass agree.

### 6.2 Culling

Refcount flood from roots (§4.1). A culled pass is reported with the resource that would have kept it alive. Mostly a convenience: renderers already use `if`s, but it makes "disconnect a debug view and everything upstream disappears" free.

### 6.3 Physical passes (merging)

Walk surviving passes in order and extend the current physical pass while all rules hold. The rules are the intersection of Vulkan local_read, Metal and Arm/Qualcomm merge conditions ([platform.md](render_graph_notes/platform.md) C1–C2):

| Rule | Break reason when violated |
| --- | --- |
| Both passes are `Raster` | `NonRasterPass` |
| Same extent and sample count | `TargetSizeMismatch` |
| Same depth attachment (or neither has one) | `DifferentDepth` |
| Slot assignments are consistent: a slot never holds two different resources | `SlotConflict` |
| The next pass does not `Sampled`/`Storage*`-read anything written inside the current physical pass | `NonLocalRead(resource)` |
| Union of color slots ≤ 8 | `AttachmentLimit` |
| Optional per-pixel budget (default 128 bit/pixel color on Mali-class, from `VK_QCOM_tile_properties` on Adreno) | `TileBudget` |
| Backend supports pixel-local reads, if the next pass uses one | `NoPixelLocalSupport` |
| Merging enabled (`r.rg.merge`) | `Disabled` |
| Next pass is not `External` | `ExternalPass` |

Inside a physical pass, consecutive members with identical attachment usage share one *step* (no barrier between them, e.g. Forward base + skybox). A member that `PixelLocalRead`s an earlier member's output starts a new step, lowered to a by-region barrier on Vulkan and to nothing on Metal.

External dependencies of all members (e.g. the shadow map sampled by lighting) are hoisted to the physical pass's prologue barrier batch.

### 6.4 Lifetimes, memoryless and the transient pool

* For each transient: first and last physical pass that touches it.
* **Memoryless-eligible** when its whole lifetime is one physical pass, its accesses are only `ColorWrite`, `DepthWrite`, `DepthTest` and `PixelLocalRead`, and it is not extracted. Lowered to `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` + `LAZILY_ALLOCATED` memory where that memory type exists (Adreno, Mali, MoltenVK), to `MTLStorageModeMemoryless` on Apple GPUs, and to an ordinary pooled image where neither exists (NVIDIA/AMD desktop). Memoryless images are allocated per (desc) and kept alive across frames; they cost no memory on tilers.
* **Pool.** Everything else is acquired from a transient image pool keyed by (format, resolved extent, mips, layers, samples, usage union). Two transients with identical keys and disjoint lifetimes in the same frame share one image (Unity's intra-frame reuse). Images return to the pool after `max_frames_in_flight` frames, as `RHIRenderTargetPool` already does. Assignment is deterministic, so the same handle maps to the same image every frame and the Vulkan descriptor-set cache keeps hitting.
* Heap aliasing is out of scope (§14 Q10).

### 6.5 Load/store inference

Per attachment of each physical pass, with the reason recorded:

| Load | when | Store | when |
| --- | --- | --- | --- |
| `Clear` | first access declares a clear | `Store` | a later pass, an imported/extracted target or a side effect reads it |
| `DontCare` | transient, no prior writer in this frame, or fully overwritten (full-screen pass declared with `b.FullyOverwrites()`) | `DontCare` | nothing reads it afterwards (memoryless always) |
| `Load` | a prior writer exists (earlier physical pass, or imported with contents) | `None` | read-only depth that must persist (Vulkan 1.3+ `STORE_OP_NONE`; Metal `DontCare` on read-only) |

### 6.6 Barrier planning

The compiler keeps a state per subresource: `{layout, last_write (stage, access), readers since last write (stage mask)}`, seeded from the image's tracked layout for imports and `Undefined` for transients. For each access in execution order:

* **Write after anything, or layout change**: emit an image barrier (or a global memory barrier for buffers) from the prior writer / readers to this access. Transients and aliased-for-reuse images transition from `UNDEFINED` (contents discarded).
* **Read after write, same layout**: memory dependency from writer to this reader's stage. Later readers of the same version in other stages are folded into the *first* barrier by look-ahead (Granite's invalidate batching), so a resource read by fragment and compute gets one barrier.
* **Read after read, same layout**: nothing.
* Barriers are batched per physical-pass boundary into one `vkCmdPipelineBarrier2`. Stages are precise (never ALL_COMMANDS or BOTTOM→TOP); the dump flags backward dependencies (fragment → vertex/compute), which cause bubbles on Mali.
* When `VK_KHR_unified_image_layouts` is present, all layouts except `UNDEFINED`/`PRESENT_SRC` collapse to `GENERAL` (desktop only; keep precise layouts on Mali, where GENERAL defeats transaction elimination).
* Metal 3: the plan is still computed (it drives the dump, validation and Metal 4 later) but lowered to nothing, because graph resources are hazard-tracked.
* Debug knob `r.rg.full_barriers`: insert a full barrier between every pass, for bisecting synchronization bugs.

### 6.7 Plan

```cpp
struct RGPhysicalPass
{
    std::vector<RGStep> steps;                 // each step: member passes + in-pass barrier kind
    std::vector<RGAttachment> attachments;     // slot, resource, load/store (+ reasons), clear value
    RGBarrierBatch prologue;                   // hoisted external dependencies
    RGBreakReason break_reason;                // why the next pass did not join
};
```

Compile caching by graph hash (Unity) is not needed at this node count; add only if the profile shows compile time (§14 Q1).

## 7. Backend lowering

### 7.1 Vulkan

* **Dynamic rendering replaces `VkRenderPass`/`VkFramebuffer`.** One `vkCmdBeginRendering` per physical pass with the union of attachments; `VkRenderingAttachmentInfo` carries the inferred load/store and clear values. No framebuffer objects, no render-pass compatibility classes, no swapchain-recreation pass list. Requires Vulkan 1.3 or `VK_KHR_dynamic_rendering` (§14 Q5).
* **Pixel-local steps** use `VK_KHR_dynamic_rendering_local_read`: attachments read pixel-locally sit in `RENDERING_LOCAL_READ` for the whole physical pass; between steps, one `vkCmdPipelineBarrier2` with `BY_REGION` and memory barriers only (COLOR_ATTACHMENT_WRITE → INPUT_ATTACHMENT_READ). Input attachment index = color slot, so `vkCmdSetRenderingInputAttachmentIndices` is the identity mapping and one shader works on Vulkan and Metal.
* **PSOs are keyed by attachment signature**, not by render pass: `{color formats per slot, depth format, samples}` of the *physical pass* plus a per-slot write mask (0 for slots the member does not write). `RHIPipelineState` holds the description; the backend pipeline is compiled lazily per signature and cached. Viewport and scissor become dynamic state set by the graph from the physical pass extent.
* **Memoryless**: `TRANSIENT_ATTACHMENT` + lazily allocated memory type, when present.
* **Synchronization**: sync2 (`vkCmdPipelineBarrier2`); buffers get global memory barriers; acceleration structures get AS_BUILD → AS_READ memory barriers.

### 7.2 Metal

* **One `MTLRenderCommandEncoder` per physical pass**, attachments = union, load/store from the plan. The encoder is created by the graph; members record into it. The existing per-`Begin` descriptor refill in `MetalRenderPass.mm` becomes the lowering of `RGPhysicalPass`.
* **Pixel-local reads** are framebuffer fetch (`[[color(slot)]]` via Slang `SubpassInput` + `input_attachment_index(slot)`). No barrier between steps: same-pixel ordering is implicit on Apple GPUs. Intel/AMD Macs have no programmable blending, so `NoPixelLocalSupport` applies there.
* **Memoryless**: `MTLStorageModeMemoryless` for eligible attachments.
* **Hazards**: keep automatic hazard tracking (Metal 3). Blit work gets its own encoder only between physical passes (today's ad hoc blit encoders can collide with an open encoder).
* **PSO**: color formats come from the physical pass signature, write mask per slot, compiled lazily per signature like Vulkan.

### 7.3 Shaders and slots

* Slots are chosen by the pass author and fixed per resource role (e.g. GBuffer packed = 0, SceneColor = 1, ViewZ = 2, Screen = 3). Fragment outputs write `SV_Target<slot>`. With fixed slots no backend needs attachment-location remapping (Metal 3 has none).
* A pass reading pixel-locally ships two variants selected by a Slang define: `SubpassInput` load, or `Texture.Load(SV_Position)` for the sampled fallback. Only the few consumers of pixel-local reads (deferred lighting, tonemap) need this.
* Metal cannot framebuffer-fetch depth. Deferred lighting currently reconstructs position from the depth buffer (`directional_lighting.ps.slang:50`), so the GBuffer pass additionally writes view-space Z into an R32F color slot that lighting reads pixel-locally (§14 Q7).

## 8. Foreign and hidden GPU work

| Work | Today | In the graph |
| --- | --- | --- |
| NRD (`RHINrdBackend::RunDispatches`) | Records inside a caller-provided compute pass; pool images invisible, full compute barriers between dispatches | `Compute` pass declaring the user inputs/outputs from `DispatchResource`; NRD's internal pools stay private to the backend |
| MetalFX temporal denoiser | `encodeToCommandBuffer` on the raw `MTLCommandBuffer` between RHI compute passes | `External` pass (no open encoder), declares its textures; prepare/resolve stay `Compute` passes |
| ImGui | `Setup(render_pass)` binds the pipeline to one `VkRenderPass`; raw `vkCmd*` leaves the tracked bind state stale | `Raster` pass with `NativeAccess()` (§4.3); ImGui Vulkan backend initialised for dynamic rendering with the screen-color format |
| TLAS build/update | Recorded during `Tick`; no AS barrier anywhere | `Copy` pass with `AccelerationStructureBuild` on the imported TLAS; path trace declares `AccelerationStructureRead` |
| BLAS build / Metal compaction | One-shot submits; Metal commits the frame's command buffer mid-frame and waits | Stays outside the frame (async one-shot), because compaction needs a CPU readback; the frame graph imports finished BLAS only |
| IBL cook on the fly | Dispatches recorded during `Tick`, `Finalize` in an end-of-frame task with its own command buffer | `Compute` passes writing imported cube maps while cooking is active; `Finalize` readback unchanged |
| Screenshot readback | Per-renderer transitions and `ReadbackFinalOutputIfRequested` | `Copy` pass with `SideEffect()`, `CopySrc` of the screen texture, `CopyDst` of an imported staging buffer |
| CPU renderer upload | Host buffer → `CopyToImage` | `Copy` pass from an imported host buffer |
| Accumulator clear (`ClearTexturePass`) | A raster clear render pass over a UAV | `Copy` pass clear, or folded into the trace dispatch (write instead of accumulate at spp 0) |
| Per-frame uploads (staging) | Recorded wherever called | Frame prologue before the graph, one barrier after |

## 9. Multi-threaded recording

The design makes parallel recording possible; enabling it is a separate, measured step (§14 Q13).

Prerequisites that the graph work delivers anyway:

* **Explicit command context.** `RHICommandContext` replaces the implicit `current_render_pass_`/`current_compute_pass_` and the global "current command buffer". It owns the command buffer/encoder, the `RHITrackedState` bind filters, the descriptor allocation cache and the timer queries. Execute lambdas receive it through the pass context.
* **Barriers and layouts resolved at compile time**, so recording threads never read or write `RHIImage` layout state.
* **PSO signatures resolved before recording**, so pipeline compilation does not happen inside worker recording (or happens under the PSO cache lock).

Parallel executor:

* Record unit = one physical pass, or a run of small consecutive physical passes grouped until an estimated cost threshold (NVIDIA: no tiny command buffers).
* Vulkan: one primary command buffer per unit from a per-(thread, frame-in-flight) pool, all submitted in one `vkQueueSubmit2` in plan order. Metal 3: one `MTLCommandBuffer` per unit, `enqueue()`d in plan order on the render thread, encoded on workers, committed in any order.
* Thread-safety work: dynamic UBO ring bump allocation becomes atomic; descriptor sets allocate from per-context caches; deferred deletion is already locked.
* Splitting *inside* a physical pass (many draws in one mesh pass) is the second step, only for passes whose record time is measured to dominate: Vulkan secondaries (`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`) or dynamic-rendering suspend/resume, Metal 3 `MTLParallelRenderCommandEncoder`. Both Vulkan suspend/resume and Metal 4 share the rule "one submit batch, nothing in between", so by-region barriers of a pixel-local step must stay in one chunk.

GPU-side concurrency is shown rather than scheduled: the dump computes dependency levels (passes with no path between them, e.g. shadow depth and GBuffer), and precise stage masks let tilers overlap one pass's binning with the previous pass's fragment work. Async compute stays a future per-pass attribute.

## 10. Visualization and diagnostics

* **Dump.** `r.rg.dump=1` writes `render_graph_<renderer>_<frame>.json` next to the logs (and a `.dot` for Graphviz): passes (kind, culled, physical pass, step, accesses with stages, CPU record ms, GPU ms), resources (desc, kind, first/last use, memoryless, pool slot, bytes), physical passes (attachments with load/store and reasons, break reason), barriers (before which pass, resource, from → to, layouts), and totals (render passes, barriers, attachment load/store bytes, transient bytes, memoryless bytes).
* **Viewer.** A small static page (`dev/render_graph_viewer.html`, no dependencies) renders the Unity-style pass × resource grid from the JSON: write/read cells, physical-pass bands, break reasons on hover, lifetime bars, memoryless badges. The same JSON feeds tests.
* **In-app panel.** An ImGui page lists passes and resources of the live graph; selecting a texture shows it on screen through the existing full-screen quad mechanism (the generalization of `RenderConfig::output_image` to any graph resource, with mip/layer and channel selection).
* **Opportunity report.** Sorted list of break reasons and `Load`/`Store` actions weighted by bytes, e.g. "`NonLocalRead(SceneColor)` in ToneMapping costs 2×7.4 MB (render_scale 0.5 upsample)". This is the tool for finding dependencies to remove.
* **GPU timing.** Timestamps per physical pass for raster and compute on both backends (render passes currently have none), feeding `PassTimingAggregator` and the dump.
* **Declaration validation (debug).** The RHI records the resources bound by PSOs drawn/dispatched in each pass (`RHIShaderResourceTable::GetBindings`); the graph asserts that every bound graph resource was declared with a compatible access. Declarations stay explicit but cannot silently drift from shader bindings.
* **Vulkan synchronization validation** (validation layer `validate_sync`) as a gate in at least one local and one CI configuration.
* **Kill switches**: `r.rg.merge`, `r.rg.memoryless`, `r.rg.cull`, `r.rg.pool_reuse`, `r.rg.full_barriers`, later `r.rg.parallel`.

## 11. RHI changes

| Change | Why | Backends |
| --- | --- | --- |
| Vulkan 1.3 baseline (or 1.1 + `KHR_dynamic_rendering` + `KHR_synchronization2`), optional `KHR_dynamic_rendering_local_read` | dynamic rendering, sync2, on-chip steps | Vulkan |
| Replace persistent `RHIRenderPass`/`RHIRenderTarget` usage with a per-frame `RHIRenderingInfo` (attachments, per-attachment load/store/clear, layouts) | graph owns render passes; no framebuffer objects | both |
| PSO compiled lazily per attachment signature; dynamic viewport/scissor | decouple PSO from render pass; merged passes | both |
| `RHICommandContext` object passed to recording code; no implicit current pass | explicit recording state; enables threads | both |
| Access-based barrier API: `ctx.Barrier(span<RHIBarrier>)` with `{resource, range, from access, to access, layout from/to, discard}`; `RHIPipelineStage` extended (AS build, ray query, indirect, host) | graph lowering; fixes storage RAW/WAW and AS barriers | Vulkan (Metal no-op) |
| Buffer and acceleration-structure accesses in barriers | untracked today | Vulkan |
| `TransientAttachment` usage mapped to `TRANSIENT_ATTACHMENT` + lazily allocated memory; Metal `Memoryless` storage | memoryless attachments | both |
| Timestamps on render passes; debug labels always on (Vulkan compute labels too) | timing and captures | both |
| Transient image pool (evolution of `RHIRenderTargetPool`, image-level, per-frame acquire/release) | graph-owned transients | common |
| Copies, clears, mip generation and readback recorded only through a context (assert when called inside an open pass) | no side-effect recording | both |
| Descriptor sets released after `max_frames_in_flight`, not end of frame; bindless dirty flush before compute passes too | correctness under the graph | Vulkan |

## 12. Worked example: Deferred frame after the graph

At `render_scale = 1`, no screenshot this frame, directional light present:

| Physical pass | Steps | Attachments (slot: resource, load → store) |
| --- | --- | --- |
| P0 Shadow | DepthPass | depth: ShadowMap, Clear → Store |
| P1 Main | (1) GBuffer; (2) Lighting + SkyBox; (3) ToneMapping; UI joins step 3 | 0: GBufferPacked Clear → DontCare, memoryless; 2: ViewZ Clear → DontCare, memoryless; 1: SceneColor DontCare → DontCare, memoryless; 3: Screen DontCare → Store; depth: SceneDepth Clear → DontCare, memoryless |
| P2 Present | quad into swapchain (stretch + pre-rotation) | 0: BackBuffer DontCare → Store |

Break reasons: P0→P1 `TargetSizeMismatch` (shadow map is 1024²) and `DifferentDepth`; P1→P2 `NonLocalRead(Screen)` because present samples with scaling. Tile budget in P1 is 16+4+8+4 = 32 B/pixel plus depth, which fits Apple7+ (128 B) and is double Arm's 128-bit recommendation, so on Mali-class tiers the compiler may split tonemap into its own pass (`TileBudget`); Adreno bin sizing is queried at runtime.

Per-pixel traffic drops to roughly 4 B store (screen) + 4 B sample and 4 B store in present, ~12 B/pixel versus ~96 B today (estimate, same accounting as §2), and ~26 MB of GBuffer/depth/scene-color memory becomes memoryless on tilers. With `render_scale < 1`, tonemap samples SceneColor bilinearly, so P1 ends after step 2 (`TargetSizeMismatch`), SceneColor is stored and GBuffer/ViewZ/depth stay memoryless. A screenshot frame without UI inserts a readback `Copy` pass between tonemap and UI, splitting P1 with reason `NonRasterPass`; that is acceptable for screenshot frames only.

Forward: P0 Shadow, P1 BasePass + SkyBox (same step, no barrier) + ToneMapping + UI with SceneDepth memoryless (SceneColor too at scale 1), P2 Present. GPU renderer: compute chain with correct storage barriers (accumulator RMW, denoiser inputs), then the shared post chain.

## 13. Migration plan

Each phase keeps all CI screenshot tests passing (FLIP vs GT unchanged; ideally bit-identical, since the graph should not change shading), plus the listed extra gate. Phases 0–2 bring no visual change by design.

| Phase | Work | Gate beyond CI screenshots |
| --- | --- | --- |
| 0 RHI groundwork | Vulkan 1.3 path, dynamic rendering, PSO attachment signatures, dynamic viewport, `RHIRenderingInfo`, `RHICommandContext`, access-based barriers incl. buffers/AS, render-pass timestamps | Vulkan sync validation clean on Forward/Deferred/GPU (it is expected to flag today's storage and AS hazards first); on-device S25 run |
| 1 Graph core | handles, builder, validation, cull, barrier planning, load/store inference (no merging yet: every pass its own physical pass), transient pool, imported/persistent resources, External/Copy passes; port all four renderers onto shared modules; delete renderer-level `Transition` calls | sync validation clean; per-renderer golden graph-shape test (pass list, barrier count, load/store per attachment) from the dump |
| 2 Visibility | JSON/DOT dump, static viewer, ImGui panel with resource viewer, opportunity report, per-pass GPU timing, debug declaration validation | viewer renders every renderer's dump; declaration validation silent |
| 3 On-chip | `PixelLocalRead`, physical-pass formation with break reasons, local_read lowering, Metal framebuffer fetch, memoryless/lazy memory, ViewZ slot, shader variants | golden graph-shape shows the §12 structure; Xcode GPU capture and Snapdragon Profiler show one render pass for P1; memory and bandwidth counters before/after on S25 and an Apple device |
| 4 Parallel recording | parallel executor per physical pass, then intra-pass split if a pass dominates | identical images; render-thread CPU time before/after on S25 and desktop |
| 5 Optional | heap aliasing, async compute, Metal 4 backend, graph-driven resize without renderer recreation | measured wins only |

## 14. Open questions

### Q1. Rebuild the graph every frame, or compile once and cache?

* Rebuild — Pro: conditional passes are `if`s (denoiser on/off, readback, sky present, output view); no invalidation logic; Frostbite/UE/Unity/Filament all do it; compile at 5–20 nodes is microseconds. Con: per-frame allocations (solved with a frame arena) and repeated work.
* Compile once + permutations (Activision, Daxa) — Pro: zero per-frame compile. Con: needs a condition key for every toggle; stale-plan bugs; built for hundreds of passes.
* **Suggestion:** rebuild every frame into an arena; add a graph-hash cache only if the dump's compile time ever shows up in a profile.

### Q2. Where do access declarations come from?

* Explicit builder calls — Pro: intent is visible (a pixel-local read or `FullyOverwrites` cannot be reflected); works for External passes. Con: can drift from the shader bindings.
* Harvested from shader reflection (`GetBindings`, SPIR-V `NonWritable`, `MTLBinding.access`) — Pro: no duplication. Con: storage bindings do not say read vs write today; unused bindings create false dependencies (UE's `ClearUnusedGraphResources`); attachments and intent are not in reflection.
* **Suggestion:** explicit declarations, with debug-build validation against the bindings actually used in the pass (§10). Keep the option to auto-fill `Sampled`/`Uniform` reads from reflection later if declaration boilerplate becomes a complaint.

### Q3. Authoring style: lambdas or pass classes?

* Setup lambda returning execute lambda (Frostbite/UE/Filament) — Pro: top-to-bottom code, captures are explicit. Con: lifetime traps with captured references (must capture by value); harder stack traces (mitigated by serial mode).
* Pass class with virtual `Setup(RGBuilder&)`/`Execute(ctx)` — Pro: matches today's `PipelinePass`. Con: per-frame data must be stored in members between setup and execute, which reintroduces hidden state and blocks parallel recording of two instances.
* **Suggestion:** keep `PipelinePass` classes for persistent state (PSOs, shaders), give each an `AddTo(RenderGraph&, inputs...) -> outputs` method that registers a setup lambda whose execute lambda captures handles by value. Remove `InitRenderResources`' render-target creation and `Render()`.

### Q4. Merging: automatic by rules, or explicit author-defined groups?

* Automatic greedy by rules (Unity, Granite without reorder) — Pro: no hand-maintained groups; break reasons tell authors what to change; kill switch for A/B. Con: surprising splits when a pass changes; Sandu reports automatic merging losing to hand placement on a shipping title (with reordering heuristics).
* Explicit groups (UE mobile, O3DE `MergeChildrenAsSubpasses`, Filament `subpassMask`) — Pro: predictable. Con: duplicate intent (the pixel-local reads already say it); groups rot when passes change.
* **Suggestion:** automatic, no reordering, with `PixelLocalRead` as the only authoring intent and a golden graph-shape test per renderer so an unintended split fails CI. This removes the search problem that makes merging NP-hard.

### Q5. Vulkan lowering and minimum version

* Dynamic rendering + `VK_KHR_dynamic_rendering_local_read` — Pro: vendor-recommended path on Mali, Adreno (tile_shading builds on it) and desktop; no render-pass objects or compatibility classes; supported by S25 Ultra driver, Mali r49+, NVIDIA, AMD, MoltenVK 1.4+. Con: needs Vulkan 1.3 (or extensions); `minSdk 31` Android devices may ship 1.1 drivers; lavapipe/SwiftShader CI support must be checked.
* Classic `VkRenderPass` subpasses — Pro: Vulkan 1.0. Con: MoltenVK splits every subpass into a separate Metal pass; Arm calls merging unpredictable; PSO compatibility pain (O3DE); a second lowering path to maintain.
* **Suggestion:** require Vulkan 1.3 core (dynamic rendering + sync2); treat local_read as a capability — without it, `NoPixelLocalSupport` keeps passes separate and uses the sampled shader variant. Needs your decision on dropping Vulkan-1.1-only Android devices, and a check of CI ICD support (lavapipe, emulator ICD).

### Q6. Fallback when pixel-local reads are unavailable

* Sampled shader variant per consumer — Pro: every device still renders; only 2–3 shaders affected. Con: two variants to keep in sync and test.
* Refuse the device / require the feature — Pro: one path. Con: excludes Intel/AMD Macs on Metal, older Android drivers, possibly CI ICDs.
* **Suggestion:** variants behind one Slang helper (`LoadPixelLocal(slot, input, pixel)`), with a CI case that forces `r.rg.merge=0` so the fallback stays covered.

### Q7. Depth input for deferred lighting on Metal

* Write view-space Z to an R32F color slot in the GBuffer pass, read pixel-locally — Pro: portable; depth itself becomes memoryless. Con: +4 B/pixel tile memory; one more MRT output.
* Keep sampling the depth buffer — Pro: no GBuffer change. Con: breaks the merge (`NonLocalRead(SceneDepth)`) on every backend, so most of the §12 win is lost.
* Vulkan-only depth local read — Pro: no extra slot on Vulkan. Con: backend divergence; needs the 1.4 depth local-read property; still requires the extra slot on Metal.
* **Suggestion:** ViewZ slot on all backends. Revisit if the tile budget becomes the binding constraint on Mali-class GPUs.

### Q8. Barrier granularity

* Precise per-resource barriers from the access state machine — Pro: required for layout transitions on Mali/Adreno without unified layouts; precise stages avoid tiler bubbles; the state machine is needed anyway for load/store and memoryless. Con: more code than global barriers.
* Global stage barriers per boundary (Blade, Aaltonen) — Pro: tiny, cheap on desktop, maps 1:1 to Metal 4. Con: layouts still needed on most of our targets; over-synchronizes independent passes.
* **Suggestion:** precise per-resource image barriers where layouts change, global memory barriers for buffers, collapse to `GENERAL` under `unifiedImageLayouts`. The same plan lowers to Metal 4 stage barriers later.

### Q9. Metal hazard tracking

* Keep automatic tracking (Metal 3 default) — Pro: zero Metal barrier code now; deployment targets (macOS 14.2, iOS 18) predate Metal 4 anyway. Con: tracking overhead; heaps/aliasing and Metal 4 need explicit sync later.
* Untracked resources + graph-emitted fences now — Pro: Metal 3 and Metal 4 paths structurally identical; enables heap aliasing. Con: more work now, and correctness risk on a backend without a sync validator as strong as Vulkan's.
* **Suggestion:** keep tracking; the compiled barrier plan is backend-neutral, so switching is a lowering change when Metal 4 or heap aliasing is adopted.

### Q10. Transient memory strategy

* Descriptor-keyed pool with intra-frame reuse + memoryless — Pro: simple; memoryless already removes the largest tiler allocations (GBuffer, depth, scene color); deterministic handle → image mapping keeps descriptor caches warm. Con: no reuse across different descriptors on desktop.
* Heap placement aliasing (Frostbite, UE) — Pro: ~45% transient memory saving in Frostbite's numbers. Con: aliasing barriers, discard rules, Metal heaps become untracked (Q9), ~5% GPU cost reported by Halcyon; little to alias at our pass count.
* **Suggestion:** pool + memoryless. Revisit when the dump's transient bytes on mobile justify it.

### Q11. Scope: what GPU work belongs to the graph?

* Everything recorded in the frame (TLAS, IBL cook, readback, uploads, clears) — Pro: complete visualization and timing; all barriers derived; no side-effect recording. Con: uploads are issued from many places (proxies, loaders) and would need deferral into a queue.
* Only render passes; the rest stays imperative — Pro: smaller change. Con: the missing AS barrier and hidden `Tick` dispatches remain; visualization is incomplete.
* **Suggestion:** TLAS build, IBL cook dispatches, readback and clears become graph passes (§8); uploads stay in a prologue with one barrier; BLAS build and Metal compaction stay outside the frame.

### Q12. Temporal and history resources

* Imported persistent resources owned by renderers — Pro: matches today (accumulator, NRD `output_history_` and denoiser outputs are read-modify-write in place; MetalFX keeps history internally); no new concept. Con: each ping-pong need writes its own swap logic.
* First-class keyed history API (`graph.History("taa", desc) -> {current, previous}`, kajiya/Activision) — Pro: automatic rotation, reset on camera cut, per-view multiplication. Con: no current user.
* **Suggestion:** imports only; add the history API with its first ping-pong user (TAA, temporal SSAO).

### Q13. When and how to record in parallel

* Now, parallel primaries per physical pass — Pro: proves the context refactor. Con: current scenes have few draws; recording is unlikely to be the bottleneck; threading bugs without a measured win.
* Later, driven by per-pass CPU record time in the dump — Pro: work lands where it pays. Con: the structural rules (side-effect-free execute, no global state) must be enforced before anyone depends on them.
* Secondaries / parallel render encoders inside passes first — Pro: helps a single heavy mesh pass. Con: Godot disabled them after driver issues; UE disables parallel execute on mobile; Activision found draw-list splitting not viable on TBDR.
* **Suggestion:** deliver the prerequisites in Phases 0–1 (explicit context, compile-time barriers, a serial-mode assert that execute lambdas touch no global recording state), measure per-pass record time, and enable parallel primaries in Phase 4 when a scene makes the render thread CPU-bound. Intra-pass splitting only after an S25 comparison of secondaries vs suspend/resume.

### Q14. Handle model

* Plain indices + declaration order — Pro: smallest; enough without reordering. Con: a write does not visibly produce a "new" value, so data flow reads less explicitly.
* Versioned handles (Filament: `write()` returns a new handle, old one invalid) — Pro: explicit data flow; enables reordering later. Con: more API surface; every write must thread the new handle.
* **Suggestion:** plain indices; module functions still return handles, which gives most of the readability.

### Q15. Visualization surface

* JSON/DOT dump + static HTML viewer — Pro: diffable, testable, works for headless CI and on-device runs (pull the file). Con: not live.
* In-app ImGui panel — Pro: live, combined with the resource viewer. Con: more UI code; does not help CI.
* **Suggestion:** dump first (it also feeds the golden tests), ImGui panel with resource viewer second.

### Q16. Resolution changes

* Graph-driven: transients re-resolve from `RenderResolution` each frame; only persistent resources reset — Pro: no renderer/proxy recreation on resize or render_scale change; removes a WaitForDeviceIdle path. Con: persistent resources (accumulator, NRD pools, MetalFX scaler) still need explicit reset logic.
* Keep recreating the renderer — Pro: proven path, simple invariants. Con: slow, and a known bug source (stale camera proxy).
* **Suggestion:** keep recreation until the graph is in (Phase 5), then switch.

### Q17. Placement and naming

* Graph in `libraries/*/renderer/graph/` (renderer-level), RHI gets the lowering primitives (`RHIRenderingInfo`, `RHICommandContext`, barriers) — Pro: RHI stays an API abstraction; graph can use `RenderResolution`, passes and config. Con: graph lowering touches RHI internals through public interfaces only.
* Graph inside the RHI (Godot) — Pro: every RHI user benefits automatically. Con: no pass-level intent; the known limits of the implicit shape.
* **Suggestion:** renderer-level `RenderGraph`, `RGBuilder`, `RGTexture`/`RGBuffer`, `RGAccess`; RHI tests (`tests/rhi`) keep using the RHI directly through `RHIRenderingInfo`.

### Q18. Reordering

* None — Pro: debugging matches source order; no heuristics; compatible with future aliasing. Con: a compute pass declared between two raster passes splits them (`NonRasterPass`), which authors must fix by hand.
* One adjacency heuristic (move a raster pass next to its compatible predecessor when no dependency forbids it; Activision mobile) — Pro: saves some splits automatically. Con: order differs from source; more code.
* **Suggestion:** none. The opportunity report tells authors which declaration to move.

## 15. Defects found during the survey

Static reading only; none reproduced. Several are exactly the classes the graph removes, which makes them good first validation targets (Vulkan sync validation should flag the first two).

1. Storage image RAW/WAW is unsynchronized on Vulkan: `Transition` returns early when old and new layout map to the same `VkImageLayout` (`libraries/source/rhi/vulkan/VulkanImage.cpp:77-80`), and StorageWrite/General both map to `GENERAL`.
2. No acceleration-structure build → ray-query read barrier exists, and `RHIPipelineStage` cannot express one.
3. Released Vulkan descriptor sets return to the free list at the end of the *current* frame (`VulkanDescriptorSetManager.cpp:251-268`); the next frame can `vkUpdateDescriptorSets` a set the previous frame's GPU work still uses.
4. Bindless dirty flush happens only in `VulkanRenderPass::Begin`, not before compute passes.
5. Metal creates ad hoc blit encoders on the frame command buffer, which is illegal while another encoder is open; Metal TLAS build commits the frame command buffer mid-frame and waits, which also undercounts frame GPU time.
6. Metal render pass ignores `clear_color` (hard-coded black) and `array_layer`.
7. Forward prepass: `pre_pass_->UpdateFrameData` is never called (`ForwardRenderer.cpp:284-290`), so its PSO table stays empty while `MeshPass::Render` indexes it (path is off by default).
8. `DirectionalLightingPass::SetIBL(nullptr)` dereferences the pointer (`DirectionalLightingPass.cpp:183-199`); Forward guards the same case.
9. ImGui's raw `vkCmd*` leaves `RHITrackedState` stale; the following present draw is correct only because its pipeline/vertex buffer/viewport happen to differ.
10. `SkyBoxPass` is initialized twice in both raster renderers; SSAO (`use_ssao`, `SSAOResource`), `BlurPass` and Forward's ray-tracing branch are dead code.
