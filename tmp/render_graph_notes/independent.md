# Independent / open-source render graphs: survey notes for a lean design

Scope: open-source and independent render graph designs, plus arguments for the "barrier-only" alternative, read with one question in mind: what should a small C++20 renderer (Vulkan + Metal; desktop, Apple Silicon, iOS, Android/Adreno) take from them? I read the source directly where it exists (default branches as of 2026-09-25). Line counts are raw newline counts of the listed files, comments included, so treat them as order-of-magnitude.

---

## TL;DR

- **Most of the value comes from four cheap features.** Each pass declares typed accesses. Passes run in declaration order. Barriers and load/store ops are derived from the access list. Passes whose outputs nobody reads are culled. Filament, kajiya, skaarj1989, and the "poor man's" graphs all stop about there, and they ship or run fine ([Filament fg](https://github.com/google/filament/tree/main/filament/src/fg), [kajiya-rg](https://github.com/EmbarkStudios/kajiya/tree/main/crates/lib/kajiya-rg/src), [Liam Tyler](https://liamtyler.github.io/posts/task_graph/)).
- **Keeping data on tile is the one tiler-specific feature worth real code.** It is also the one where "automatic" does worst:
  - Granite's greedy subpass merging is roughly 100 lines of `should_merge` plus a reorder heuristic ([render_graph.cpp](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)).
  - A 2024 paper found Granite-style merging worse than hand placement on a shipping mobile title ([Sandu & Shcherbakov, WSCG 2024](http://wscg.zcu.cz/WSCG2024/JWSCG-2024/B43-2024.pdf)).
  - Filament and Unity URP instead let the pass author opt in explicitly: Filament through `subpassMask` / `SUBPASS_INPUT` ([DriverEnums.h](https://github.com/google/filament/blob/main/filament/backend/include/backend/DriverEnums.h)), Unity through `SetInputAttachment` ([Unity manual](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-framebuffer-fetch.html)).
- **Heavy features cost more than they return at small scale.** Pass reordering, async compute, cross-queue ownership, SSA/futures IRs, precompiled graphs, per-subresource tracking, scripting and editors all fall in this bucket. Their authors say so themselves:
  - Daxa: reordering conflicts with aliasing ([Daxa wiki](https://docs.daxa.dev/wiki/taskgraph-how-why/)).
  - Granite: exclusive queue ownership "will be brutal" ([Granite blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).
  - Bevy deleted its execution-only graph in 0.19 ([Bevy 0.19](https://bevy.org/news/bevy-0-19/)).
- **Barriers are getting simpler.** VK_KHR_unified_image_layouts (2025) removes most layout transitions ([Khronos](https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation)). Metal 4 barriers are stage-to-stage with no resource list ([WWDC25 254](https://developer.apple.com/videos/play/wwdc2025/254/)). Aaltonen argues resource lists in barriers are legacy ([No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api)).
  - The graph can therefore emit a coarse "stage → stage" barrier per pass boundary, and the resource-level knowledge goes to lifetimes, load/store and transients.
  - The main caveat: on tilers, what costs you is a render pass break, not a barrier ([Blade study](https://arxiv.org/abs/2607.26506) says so and explicitly excludes tilers).

---

## 1. Granite (Hans-Kristian Arntzen / Themaister)

Sources: [blog, 2017](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/), [render_graph.hpp](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.hpp) (1,094 lines), [render_graph.cpp](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp) (3,876 lines). The repo is still active (last push 2026-09-25).

**Data model.** `RenderGraph` owns `RenderPass`es and `RenderResource`s (texture/buffer), all addressed by **string name**. Each pass has a queue flag: `RENDER_GRAPH_QUEUE_GRAPHICS_BIT`, `COMPUTE_BIT` or `ASYNC_COMPUTE_BIT`. Resources carry `AttachmentInfo`:

- a `SizeClass` of `Absolute`, `SwapchainRelative` or `InputRelative`, with `size_relative_name`;
- `size_x`/`size_y` scale, format, samples, levels, layers;
- flags `PERSISTENT`, `UNORM_SRGB_ALIAS`, `MIPGEN` ([hpp L154-167](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.hpp)).

**API sketch** (method names from the header):

```cpp
auto &gbuf = graph.add_pass("gbuffer", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
gbuf.add_color_output("albedo", albedo_info);
gbuf.add_color_output("normal", normal_info);
gbuf.set_depth_stencil_output("depth", depth_info);

auto &light = graph.add_pass("lighting", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
light.add_attachment_input("albedo");            // input attachment -> subpass-mergeable
light.add_attachment_input("normal");
light.set_depth_stencil_input("depth");
light.add_color_output("hdr", hdr_info, "emissive"); // read-modify-write: "emissive" aliases into "hdr"
light.add_history_input("hdr");                    // previous frame's hdr
light.set_build_render_pass([&](Vulkan::CommandBuffer &cmd) { /* draws */ });

graph.set_backbuffer_source("tonemapped");
graph.bake();                   // once / on change
graph.setup_attachments(device, &swapchain_view);
graph.enqueue_render_passes(device, composer);   // every frame
```

**Dependencies.** Reads and writes are declared by name. Read-modify-write is expressed as `add_color_output(name, info, input)`, which creates a new logical resource aliased to the input. Baking linearizes the graph with a **bottom-up recursive traversal** from the backbuffer writer and flags merge candidates along the way ([blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).

**Reordering.** `reorder_passes` greedily picks the next pass that maximizes its distance from its dependencies, to avoid "hard barriers". A merge candidate gets `overlap_factor = ~0u`, so it always goes next. The comment in the code: "This is very inefficient, but should work okay for a reasonable amount of passes … Clarity in the algorithm is pretty important" ([cpp L2872+](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)).

**Subpass merging.** `build_physical_passes` walks the ordered list and extends a physical pass while `should_merge(prev, next)` holds for every pass already in the group:

- Refused when the two passes are on different queues, or either is compute.
- Refused when the device quirk `merge_subpasses` is false.
- Refused when the previous pass needs mipgen.
- Refused when `next` samples (non-locally) anything `prev` wrote: a texture input, storage buffer, storage image, or scaled blit.
- Refused when the depth attachments differ.
- Accepted only if at least one of these keeps data on tile: `next` uses a `prev` color output as a color input, shares depth, or reads via input attachment.

That is about 130 lines ([cpp L1221-1392](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)). Merged passes become Vulkan subpasses, and the barriers between them become `VkSubpassDependency` with BY_REGION ([blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).

**Transients / tile memory.** `build_transients` marks every non-buffer, non-storage image without history as transient. It then clears the flag if any reader or writer lives in a different physical pass. Per-vendor quirks decide whether transient color/depth is used ([cpp L954-1018](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)). Transient images get LAZILY_ALLOCATED memory, because "tile-based renderers can avoid allocating physical memory for the attachment if you never actually write to it" ([blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).

**Aliasing.** `build_aliases` computes the first/last read and write pass for each physical resource. Two resources alias only if they have the *same dimensions/format*, their ranges don't overlap, and neither has history. Ownership is handed over with a barrier to layout UNDEFINED ([cpp L1548+](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp), [blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)). This is image-level aliasing (reusing the VkImage), not heap placement, so it needs no memory aliasing barriers.

**Barriers.** Each pass gets an *invalidate* list (before) and a *flush* list (after). At run time a per-physical-resource state tracks the last write stages/access, per-stage invalidation and the current layout. The graph walks ahead to batch look-alike invalidates from future passes (`physical_pass_handle_invalidate_barrier_lookahead`) ([cpp L2324-2378](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)). The code uses sync2 `VkPipelineStageFlags2`.

**Async compute.** Semaphores handle cross-queue edges. Shared images use CONCURRENT sharing, deliberately, because "Dealing with EXCLUSIVE will be brutal, because now we have to consider read-after-read barriers as well" ([blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).

**History.** `add_history_input(name)` sets `physical_image_has_history`, which disables transient and alias for that image. `setup_attachments` then swaps the current and history images every frame ([cpp L937-949, L2705-2708](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)).

**Multithreading.** Recording is split into two timelines:

1. A **serial CPU timeline** (`physical_pass_handle_cpu_timeline`) resolves barrier state and enqueues per-pass prepare tasks.
2. A **parallel GPU timeline** (`physical_pass_handle_gpu_timeline`) creates one task per physical pass. Each task requests its own command buffer, emits pre-pass barriers, records, and ends the command buffer in-thread.
3. Submission then runs in order ([cpp L2380-2403, L2522-2575](https://github.com/Themaister/Granite/blob/master/renderer/render_graph.cpp)).

Subpass contents are always INLINE, so there are no secondary command buffers.

**Conditional work.** `RenderPassInterface::need_render_pass()` and `render_pass_is_conditional()` let a baked graph skip passes per frame without re-baking.

**Assessment.** This is the most complete independent answer to (b) + (c). It pays for that with string-keyed resources, a baking pipeline of about 7 stages, heuristic reordering, and per-vendor quirk switches. The parts most worth copying are the `should_merge` rules, the transient rule, and the CPU/GPU two-timeline split.

---

## 2. Google Filament FrameGraph (`filament/src/fg/`)

Sources: [FrameGraph.h](https://github.com/google/filament/blob/main/filament/src/fg/FrameGraph.h), [FrameGraph.cpp](https://github.com/google/filament/blob/main/filament/src/fg/FrameGraph.cpp), [PassNode.cpp](https://github.com/google/filament/blob/main/filament/src/fg/PassNode.cpp), [DependencyGraph.cpp](https://github.com/google/filament/blob/main/filament/src/fg/DependencyGraph.cpp). About 4,900 lines over 27 files; still active (commit 2026-08-20 "optimize FrameGraph arena memory footprint").

**Data model.** A generic `DependencyGraph` (nodes + edges + refcount) holds `PassNode` and `ResourceNode`. `FrameGraphId<T>` handles are **versioned**: `read()` and `write()` return a *new* handle, and "The input handle is no-longer valid" ([FrameGraph.h](https://github.com/google/filament/blob/main/filament/src/fg/FrameGraph.h)). Resources are typed through a `T::Descriptor` / `T::SubResourceDescriptor` / `T::Usage` concept (`FrameGraphTexture`). Subresources (mip or layer views) are created with `createSubresource(parent, name, desc)`. A `Blackboard` passes handles between modules by name.

**API sketch** (from the header doc comment):

```cpp
struct Data { FrameGraphId<FrameGraphTexture> color; };
auto& pass = fg.addPass<Data>("Tonemap",
    [&](FrameGraph::Builder& b, Data& d) {           // setup: synchronous
        auto in  = b.sample(hdr);                     // read(..., SAMPLEABLE)
        d.color  = b.createTexture("ldr", {.width = w, .height = h, .format = RGBA8});
        d.color  = b.declareRenderPass(d.color);      // write(COLOR_ATTACHMENT) + render target
    },
    [=](FrameGraphResources const& r, Data const& d, DriverApi& driver) { // execute: captures by copy
        auto rt = r.getRenderPassInfo();
        driver.beginRenderPass(rt.target, rt.params); /* ... */ driver.endRenderPass();
    });
fg.present(pass->color);   // keep alive
fg.compile(); fg.execute(driver);
```

Imported resources come in through `import()` / `ImportDescriptor`, including imported render targets such as the swapchain, with `keepOverrideStart/End` ([FrameGraphRenderPass.h](https://github.com/google/filament/blob/main/filament/src/fg/FrameGraphRenderPass.h)). `sideEffect()` pins a pass so it is never culled, and writing an imported resource implies a side effect. `forwardResource()` redirects one resource onto another.

**Compile.**

1. `DependencyGraph::cull` refcounts nodes by their outgoing edges and pops zero-ref nodes from a stack. That takes about 40 lines ([DependencyGraph.cpp](https://github.com/google/filament/blob/main/filament/src/fg/DependencyGraph.cpp)).
2. Surviving passes are `stable_partition`ed and keep **declaration order**, with no reordering.
3. Each resource's first and last active pass get `devirtualize`/`destroy` lists ([FrameGraph.cpp L141-249](https://github.com/google/filament/blob/main/filament/src/fg/FrameGraph.cpp)).
4. Usage bits are accumulated across all accesses, so a texture is created with exactly the union of the usages it needs.

**Render passes and tilers.** `RenderPassNode::resolve` derives load/store behavior per attachment from the graph ([PassNode.cpp](https://github.com/google/filament/blob/main/filament/src/fg/PassNode.cpp)):

- `discardStart` if there is no prior writer ("Discard at the start if this attachment has no prior writer");
- `discardEnd` if no later reader;
- `readOnlyDepthStencil` if nobody writes;
- `clear` implies discardStart.

That is exactly the load/store derivation tilers need, in about 60 lines. Nothing is merged automatically. For multi-subpass rendering, a single FG pass declares `SUBPASS_INPUT` usage and sets `params.subpassMask = 1`, then calls `driver.nextSubpass()`. Filament uses this for color grading and custom MSAA resolve ([DriverEnums.h](https://github.com/google/filament/blob/main/filament/backend/include/backend/DriverEnums.h) "For now only 2 subpasses are supported"; [PostProcessManager.cpp](https://github.com/google/filament/blob/main/filament/src/PostProcessManager.cpp)).

**Barriers.** The FrameGraph emits **none**. Filament's backend tracks state itself: the Vulkan backend's `VulkanTexture::transitionLayout` keeps per-subresource layouts ([VulkanTexture.h](https://github.com/google/filament/blob/main/filament/backend/src/vulkan/VulkanTexture.h)). The graph only guarantees ordering, lifetimes, usage and load/store.

**Aliasing.** There is no in-frame memory aliasing. `devirtualize` and `destroy` go through a `TextureCacheInterface`, which pools textures and render targets by descriptor across frames.

**History.** Not in the FrameGraph. Filament keeps history textures outside the graph and imports them.

**Multithreading.** `execute` issues commands into Filament's `DriverApi` command stream. The stream is replayed on a separate driver thread, so the graph itself records serially.

**Visualization.** `export_graphviz` writes a dot file in which each pass label carries `refs`, and `S:/E:/C:` (discardStart/End/clear) flags per render target ([PassNode.cpp `graphvizify`](https://github.com/google/filament/blob/main/filament/src/fg/PassNode.cpp)). `fgviewer` is a live web app that "displays active passes and resource usage" and can read back textures ([libs/fgviewer](https://github.com/google/filament/tree/main/libs/fgviewer)).

**Assessment.** This is the cleanest *production* design for a small team. It uses typed, versioned handles, keeps declaration order, culls by refcount, derives load/store from graph edges, and leaves barriers to the backend. The subpass story is explicit and small.

---

## 3. kajiya (Tomasz Stachowiak / Embark, Rust)

Sources: [graph.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/graph.rs), [temporal.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/temporal.rs), [hl.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/hl.rs), [taa.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya/src/renderers/taa.rs). The kajiya-rg crate is about 4,000 lines in 10 files, including the binding helpers. The repo is **archived** and the README says "This project is no longer maintained" ([README](https://github.com/EmbarkStudios/kajiya)).

**Data model.** `RenderGraph` holds resources (created, imported, or swapchain) and `RecordedPass { read: Vec<PassResourceRef>, write: Vec<..>, render_fn }`. Each ref carries a `vk_sync::AccessType` plus a sync type, either `AlwaysSync` or `SkipSyncIfSameAccessType` ([graph.rs L1160-1211](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/graph.rs)).

**API sketch** (real code from TAA):

```rust
let (mut out, history) = self.temporal_tex.get_output_and_history(rg, desc); // ping-pong temporal
let mut reprojected = rg.create(desc);
SimpleRenderPass::new_compute(rg.add_pass("reproject taa"), "/shaders/taa/reproject_history.hlsl")
    .read(&history)
    .read(reprojection_map)
    .read_aspect(depth_tex, vk::ImageAspectFlags::DEPTH)
    .write(&mut reprojected)
    .constants((/* ... */))
    .dispatch(reprojected.desc().extent);
```

**The notable idea:** declaring the access *is* binding the resource. `SimpleRenderPass::read/write` push the ref onto `state.bindings` in declaration order, and those map to shader descriptor slots ([hl.rs L256-336](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/hl.rs)). Nothing is written twice. This answers the "you end up specifying everything twice" complaint in §11. `write_no_sync` opts out of a WAW barrier when concurrent writes are intended.

**Barriers.** Passes run in declaration order in one command buffer. Before each pass, `transition_resource` compares the resource's current `access_type` with the requested one and records a `vk_sync` image or buffer barrier. If the access type is unchanged and the sync type allows it, the barrier is skipped (`RG_ALLOW_PASS_OVERLAP`). One upfront step also transitions every resource to its first-use access, "which would otherwise occur with temporal resources". The code says `// TODO: optimize the barriers` ([graph.rs L812-1110](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/graph.rs)).

**Aliasing.** None (`// TODO: alias resources`, [graph.rs L552](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/graph.rs)). A `TransientResourceCache` recycles images and buffers by descriptor across frames (`release_resources`).

**Temporal resources.** `TemporalRenderGraph` wraps `RenderGraph` plus a `HashMap<TemporalResourceKey, TemporalResourceState>` whose states are `Inert`, `Imported` and `Exported`. `get_or_create_temporal(key, desc)` imports the persistent image with its last known access type. At frame end the resource is exported and its final access state is stored, so the next frame's barrier is correct ([temporal.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/temporal.rs)). `PingPongTemporalResource` sits on top ([taa.rs](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya/src/renderers/taa.rs)).

**Subpasses / multithreading / visualization.** None of these. Recording is single-threaded into a main and a presentation command buffer. `hook_debug_pass` lets you intercept any pass's output image for debug viewing ([graph.rs L612](https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/kajiya-rg/src/graph.rs)).

**Assessment.** This is the purest "immediate-ish" graph: rebuilt every frame, no compile step, just-in-time barriers. Its temporal-by-key API is the nicest seen for history resources. It was desktop RT research code and never cared about tilers.

---

## 4. vuk (martty) — declarative Vulkan rendergraph with futures

Sources: [README](https://github.com/martty/vuk), [RenderGraph.hpp](https://github.com/martty/vuk/blob/master/include/vuk/RenderGraph.hpp), [IRPasses.cpp](https://github.com/martty/vuk/blob/master/src/IRPasses.cpp), [Backend.cpp](https://github.com/martty/vuk/blob/master/src/runtime/vk/Backend.cpp), [04_texture.cpp](https://github.com/martty/vuk/blob/master/examples/04_texture.cpp). The core (IR + compiler + backend + RG API) is about 7,600 lines, not counting the rest of the runtime. Last push 2026-08-01.

**Data model.** Current vuk is an **SSA IR**. `vuk::Value<T>` is a future of a resource: images, buffers, even pipelines through `compile_pipeline`. A pass is a typed function whose parameter types carry the access.

**API sketch:**

```cpp
auto pass = vuk::make_pass("04_textured_cube",
    [](vuk::CommandBuffer& cb, VUK_IA(vuk::eColorWrite) color, VUK_IA(vuk::eDepthStencilRW) depth) {
        cb.set_viewport(0, vuk::Rect2D::framebuffer()) /* ... */ .draw_indexed(/*...*/);
        return color;                          // returned value = new version flowing onward
    });
auto depth = vuk::declare_ia("04_depth"); depth->format = vuk::Format::eD32Sfloat;
depth = vuk::clear_image(std::move(depth), vuk::ClearDepthStencil{1.0f, 0});
auto out = pass(std::move(target), std::move(depth));   // builds IR; nothing runs yet
// out.wait(allocator, compiler) / submit(...) forces compilation + execution
```

`lift_compute(pipeline)` turns a reflected compute shader into a callable. The access for each binding comes from reflection (`non_writable` → `eComputeRead`, and so on) ([RenderGraph.hpp L446-510](https://github.com/martty/vuk/blob/master/include/vuk/RenderGraph.hpp)).

**Compile.** IR passes include:

- `reify_inference`, which infers missing image extent, format and samples from use ("framebuffer inference");
- `queue_inference`, which propagates forward and backward over queue domains;
- linearization into domain groups ([IRPasses.cpp L700-1175](https://github.com/martty/vuk/blob/master/src/IRPasses.cpp)).

Sync is lowered to sync2 memory and image barriers per use ([Backend.cpp L660-680](https://github.com/martty/vuk/blob/master/src/runtime/vk/Backend.cpp)).

**Subpasses.** The README still lists "Automatically deduces renderpasses, subpasses and framebuffers" ([README](https://github.com/martty/vuk)). The current backend builds **one subpass per pass**, with `loadOp = LOAD`, `storeOp = STORE` (or NONE when read-only), `dependencyCount = 0` and the comment "we use barriers" ([Backend.cpp L681-740](https://github.com/martty/vuk/blob/master/src/runtime/vk/Backend.cpp)). So automatic subpass inference is not in the IR rewrite today.

**Multithreading.** No parallel recording was found in the backend.

**Visualization.** `GraphDumper.cpp` dumps the IR.

**Assessment.** Futures and inference give a very ergonomic API, but a compiler-sized implementation. The typed-parameter access declaration (`VUK_IA(eColorWrite)`) is a neat way to put the access in the signature. The rest is over budget for a lean renderer.

---

## 5. Daxa TaskGraph (Ipotrick)

Sources: [TaskGraph — How and Why](https://docs.daxa.dev/wiki/taskgraph-how-why/), [TaskGraph from the Ground Up](https://docs.daxa.dev/wiki/taskgraph-bottom-up/), [impl_task_graph.cpp](https://github.com/Ipotrick/Daxa/blob/master/src/utils/impl_task_graph.cpp). About 9,700 lines including a 122 KB ImGui/ImPlot debug UI; very active (last push 2026-09-24).

**Data model.** `TaskImageView`/`TaskBufferView` are virtual handles with a lifetime of TRANSIENT, PERSISTENT, PERSISTENT_DOUBLE_BUFFER or EXTERNAL. Tasks are Raster, Compute or RayTracing, and attachments are declared per access stage. The graph is **recorded once and executed many times**: "static during gameplay, reconfigurable during menus/loading" ([how-why](https://docs.daxa.dev/wiki/taskgraph-how-why/)).

**API sketch:**

```cpp
tg.add_task(daxa::Task::Compute("rtao spatial denoise")
    .reads(task_rtao_trace).writes(task_rtao_spatial)
    .executes([=](daxa::TaskInterface ti) { ti.recorder.set_pipeline(p); ti.recorder.dispatch(); }));
// or shared shader/C++ attachment "heads":
// DAXA_DECL_COMPUTE_TASK_HEAD_BEGIN(H) DAXA_TH_IMAGE_ID(CS::WRITE, REGULAR_2D, color) ...
tg.submit({}); tg.present({}); tg.complete({});   // once
tg.execute({});                                    // per frame
```

Task heads are macros shared between shader and C++. They produce a push-constant "attachment blob", so as in kajiya the declaration doubles as binding ([bottom-up](https://docs.daxa.dev/wiki/taskgraph-bottom-up/)).

**Barriers and batching.** Scheduling starts from a "Min-Schedule": "all tasks are inserted into batches as early as possible", giving the minimal batch count. Later optimizations only move tasks between batches, and a batch never crosses a submit boundary ([impl_task_graph.cpp L2356-2524](https://github.com/Ipotrick/Daxa/blob/master/src/utils/impl_task_graph.cpp)). Each batch boundary gets one barrier set.

- `writes_concurrent` lets writers opt out of mutual ordering.
- `NullTaskImage` removes false dependencies for disabled features.
- "Sync tracking is not at subresource granularity."

Knobs are `reorder_tasks`, `optimize_transient_lifetimes` and `alias_transients` (the last defaults to false) ([bottom-up](https://docs.daxa.dev/wiki/taskgraph-bottom-up/)).

**Async compute.** Tasks set `.uses_queue(QUEUE_COMPUTE_0)`. Explicit `submit()` points are the convergence points where semaphores go.

**Honest caveats.** The docs say:

- "Pass reordering may conflict with memory aliasing goals".
- Recompiling is required to change the pass set.
- Don't use the TaskGraph for "simple one-off operations, static asset data" ([how-why](https://docs.daxa.dev/wiki/taskgraph-how-why/), [bottom-up](https://docs.daxa.dev/wiki/taskgraph-bottom-up/)).

**Assessment.** Its batch model (ASAP levels, one barrier batch per level) is a good mental model. It is desktop/bindless-oriented, with no tiler render-pass merging, and the implementation is large.

---

## 6. NVIDIA Falcor RenderGraph

Sources: [tutorial 02](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/tutorials/02-implementing-a-render-pass.md), [tutorial 03](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/tutorials/03-creating-and-editing-render-graphs.md), [render-passes.md](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/render-passes.md), [ResourceCache.cpp](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/RenderGraph/ResourceCache.cpp), [RenderGraphExe.cpp](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/RenderGraph/RenderGraphExe.cpp). About 5,800 lines including 61 KB of UI code.

**Data model.** This is a *module* graph of plugin passes with named I/O fields. Each pass implements `reflect()` to declare fields and `execute(RenderContext*, const RenderData&)`:

```cpp
RenderPassReflection ExampleBlitPass::reflect(const CompileData&) {
    RenderPassReflection r; r.addInput("input", "the source texture"); r.addOutput("output", "the destination texture"); return r;
}
void ExampleBlitPass::execute(RenderContext* ctx, const RenderData& rd) {
    ctx->blit(rd.getTexture("input")->getSRV(), rd.getTexture("output")->getRTV());
}
```

Graphs are Python scripts (`g.addPass(...)`, `g.addEdge("ImageLoader.dst", "MyBlitPass.input")`, `g.markOutput(...)`) or are built in a node editor that live-edits the running Mogwai app ([tutorial 03](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/tutorials/03-creating-and-editing-render-graphs.md)). Fields can be `Optional` or `Persistent`, where Persistent means "retain its data between calls" ([render-passes.md](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/render-passes.md)).

**Compile and execute.**

- The compiler computes field lifetimes and registers inputs as aliases of the outputs they connect to.
- `allocateResources` then **creates every resource separately**, with no memory aliasing, filling size and format defaults from the graph ([ResourceCache.cpp](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/RenderGraph/ResourceCache.cpp)).
- Execution is a linear loop over the execution list ([RenderGraphExe.cpp](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/RenderGraph/RenderGraphExe.cpp)).
- Barriers are left to `RenderContext` state tracking.

**Assessment.** Falcor optimizes for research iteration: hot-swappable passes, scripting, and an editor for *visualizing and wiring*. The editor is its real strength. For performance it does nothing: no aliasing, no merging, no parallel recording. It is a good model for a debug *viewer*, not for the runtime.

---

## 7. Bevy render graph — and its removal

Sources: [Bevy 0.19 release notes](https://bevy.org/news/bevy-0-19/), [0.18→0.19 migration](https://bevy.org/learn/migration-guides/0-18-to-0-19/), [earlier summary (0.13)](https://hackmd.io/@bevy/rendering_summary), [issue #5062 "Render Graph as Systems"](https://github.com/bevyengine/bevy/issues/5062).

- **Before.** Bevy's graph was an *execution* graph: `Node::run(&self, graph_ctx, render_ctx, world)` with nodes, edges and slots, plus sub-graphs per camera (`ViewNode`, `CameraDriverNode`). Resources were **not** declared; wgpu tracks and places barriers. "Each node represents a self-contained unit of work… The edges specify dependencies" ([hackmd summary](https://hackmd.io/@bevy/rendering_summary)).
- **Bevy 0.19 (PR #22144, @tychedelia).** "Bevy replaced the `RenderGraph` architecture with ECS schedules. Render passes are now regular systems running in schedules like `Core3d` and `Core2d`". Ordering is `.after(Core3dSystems::MainPass)`. The motivation given: "The old approach required substantial boilerplate", and ECS schedules "became capable of expressing the render graph pattern more naturally". Future parallel command encoding via read-only schedules is noted as an opportunity ([Bevy 0.19](https://bevy.org/news/bevy-0-19/)).
- **Lesson.** A graph that only orders callbacks, with no resource declarations, adds nothing a job scheduler doesn't already do, so it got deleted. A graph earns its keep only when it knows resources: lifetimes, barriers, load/store, transients.

---

## 8. Engines without a render graph (Wicked, The Forge, Diligent), plus Godot and HypeHype

- **Wicked Engine.** There is no render graph ([docs](https://github.com/turanszkij/WickedEngine/blob/master/Content/Documentation/WickedEngine-Documentation.md)).
  - Barriers are manual (`GPUBarrier` memory, image and buffer barriers), plus implicit render-pass transitions via `RenderPassImage::layout_before/layout/layout_after`, "that works like an IMAGE_BARRIER, but can be more optimal".
  - "Missing a barrier could lead to corruption … The debug layer will help".
  - Multithreading: each thread takes a `CommandList` from the thread-safe `BeginCommandList()`, and "command lists will be submitted in the order they were retrieved".
  - Async compute: `BeginCommandList(QUEUE_COMPUTE)` plus `WaitCommandList(a, b)`, a GPU wait at command-list granularity.
  - Aliasing is manual: `CreateTexture(desc, data, tex, alias, alias_offset)` ([wiGraphicsDevice.h](https://github.com/turanszkij/WickedEngine/blob/master/WickedEngine/wiGraphicsDevice.h)).
- **The Forge.** There is no render graph (no such file in the tree). Barriers are explicit through `cmdResourceBarrier(cmd, bufferBarriers, textureBarriers, rtBarriers)` with `mCurrentState`/`mNewState`. Load/store is explicit through `BindRenderTargetsDesc`, and there is a `TEXTURE_CREATION_FLAG_ON_TILE` "Use on-tile memory to store this texture" ([IGraphics.h](https://github.com/ConfettiFX/The-Forge/blob/master/Common_3/Graphics/Interfaces/IGraphics.h)).
- **Diligent.** There is no render graph. It offers two modes: engine-tracked automatic state transitions (`RESOURCE_STATE_TRANSITION_MODE_TRANSITION`), or explicit `TransitionResourceStates` with VERIFY/NONE modes ([Diligent blog](https://diligentgraphics.com/2018/12/09/resource-state-management/)). "Automatic state transitions are not thread safe", so multithreaded recording must transition explicitly up front ([gamedev.net article](https://www.gamedev.net/articles/programming/graphics/a-practical-approach-to-managing-resource-states-in-vulkan-and-direct3d12-r5027/)). Render passes and subpasses are explicit API objects ([Tutorial19](https://github.com/DiligentGraphics/DiligentSamples/tree/master/Tutorials/Tutorial19_RenderPasses)).
- **Godot 4.3: an invisible graph.** RenderingDevice keeps its immediate API but *records into a DAG behind the scenes*: "its construction is completely invisible to the programmer" ([Godot blog](https://godotengine.org/article/rendering-acyclic-graph/)).
  - Draw lists and compute lists are single nodes, about 300 nodes per frame.
  - Reads are tracked per resource. Commands are sorted into levels, and same-level commands are grouped by type and barrier-batched.
  - Results: 5–15% frametime improvement, less than 1% CPU overhead.
  - Secondary command buffers for parallel draw-list recording are "currently disabled due to hardware compatibility issues".
  - The design is inspired by Muratov (§9).
- **HypeHype (Aaltonen, SIGGRAPH 2023).** "When a render pass begins, we transition the render target to writable layout. At the end of the render pass, we transition it back to sampled texture layout. This way all the textures … are always in sampler readable layout. We never need to do per-draw call resource tracking at all" ([slides](https://advances.realtimerendering.com/s2023/AaltonenHypeHypeAdvances2023.pdf)). The same talk says to "minimize the amount of render passes" on mobile. A shipping mobile renderer (Vulkan, Metal and WebGPU through the Hyper RHI) thus uses a *convention* rather than a graph.

---

## 9. Small reference implementations and writeups

- **skaarj1989/FrameGraph** ([repo](https://github.com/skaarj1989/FrameGraph)). About 1,550 lines, renderer-agnostic, faithful to Frostbite's model.
  - A resource type `T` supplies `Desc`, `create`, `destroy`, optional `preRead`/`preWrite(desc, flags, ctx)`, and `toString`. The per-access `uint32_t flags` is opaque to the graph; the author packs binding set/slot, stage, and attachment index into it, and `preRead`/`preWrite` "build DescriptorSet tables, insert barriers" ([README](https://github.com/skaarj1989/FrameGraph/blob/master/README.md)).
  - `compile()` does refcount culling. `execute()` creates transients at first use and destroys them after the last ([FrameGraph.cpp](https://github.com/skaarj1989/FrameGraph/blob/master/src/FrameGraph.cpp)).
  - A Blackboard carries handles between modules.
  - Visualization: `std::ofstream{"fg.dot"} << fg;`, custom writers (JSON), and a web viewer at https://skaarj1989.github.io/FrameGraph/.
  - Why it is elegant: the barrier and binding policy lives entirely in the resource type's hooks, so the graph core stays tiny.
- **Pavlo Muratov, "Organizing GPU Work with DAGs"** ([2020](https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3)).
  - DFS topological sort with cycle detection; longest-path **dependency levels**, where passes in a level may run in any order.
  - For multi-queue work, a "Sufficient Synchronization Index Set" keeps only the closest cross-queue dependency and culls transitively covered fences.
  - Transitions go to the "most competent queue".
  - It forbids multiple writers per resource ("each render pass must register its own new output"), and temporal reprojection uses ping-pong.
  - Companion post on [GPU memory aliasing](https://levelup.gitconnected.com/gpu-memory-aliasing-45933681a15e). This is the design Godot 4.3 adopted.
- **Riccardo Loggini, "Render Graphs"** ([2021](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)). A survey of Frostbite, UE4 RDG and Anvil, with D3D12 placed-resource aliasing ("more than 50%" savings), dependency levels, and parallel command-list recording per level. Main caution: fences are expensive, so batch.
- **Traverse Research (Breda)** ([update post](https://blog.traverseresearch.nl/an-update-to-our-render-graph-17ca4154fd23)).
  - Versioned resources: a read names a version, a write bumps it.
  - Artificial read→write edges prevent WAR races.
  - BFS from outputs gives dead-stripping, and passes are grouped by depth.
  - Async compute and "automatic resource transitions" are left for later.
- **Liam Tyler, "A Poor Man's Render Graph"** ([2024](https://liamtyler.github.io/posts/task_graph/)).
  - It deliberately does **no reordering, no pruning, no per-frame rebuild, one queue**, because "at least for your personal projects, you were probably never bothered by suboptimal task ordering, had no unreferenced render passes, and had a lot of it single-threaded already".
  - The only optimization kept is aliasing, treated as "2D rectangle packing" (x = task index, y = memory).
  - Barriers come from per-resource `{currLayout, prevTask, prevState}` and the R→R/R→W/W→W/W→R cases.
  - "If you're writing your first task graph system, you absolutely shouldn't start with any of those things."
- **Deep Spark "Frame Graph" series** (2026). The [MVP post](https://stoleckipawel.dev/posts/frame-graph-build-it/) covers declare → compile to a `CompiledPlan` → execute as playback.
  - Build steps: versioning, then Kahn's sort, then a backward cull from Present, then precomputed barriers, then greedy free-list aliasing.
  - It is about 90 lines for the scaffold, ~260 with barriers, and ~500 with aliasing. It rebuilds every frame on a single queue and a single command list.
  - The [production post](https://stoleckipawel.dev/posts/frame-graph-production/) (Feb 2026): "If your project has a fixed pipeline with 3–4 passes that will never change, the overhead of a graph compiler is wasted complexity."
- **apoorvaj.io "Render graphs"** ([2020](https://apoorvaj.io/render-graphs-1)). Explicitly **no lambdas**: "Code runs in the same order that it's written in, and lifetimes become very straightforward." The admitted weak spot is that declaration and invocation order must match, with no compile-time enforcement.
- **Our Machinery** ([2017](https://ruby0x1.github.io/machinery_blog_archive/post/high-level-rendering-using-render-graphs/index.html)).
  - `setup_pass` / `execute_pass(inst, sort_key, commands, graph)`.
  - Passes run on a job system in any CPU order. GPU order is restored by **sort keys** when command buffers are merged, which "completely decouple[s] the execution order of `execute_pass()` from the scheduling of GPU commands".
  - Barriers, copies and load/discard/clear are resolved before `execute_pass`.
- **Gaijin daFrameGraph (Dagor Engine, open source)** ([repo](https://github.com/GaijinEntertainment/DagorEngine/tree/main/prog/gameLibs/render/daFrameGraph), about 226 files / 2 MB).
  - Resource verbs are *create / modify / read / rename*. Modifiers are mutually unordered, and every modifier precedes every reader.
  - `historyFor(R)` reads last frame and "History Reads Create NO Edges".
  - Pruning runs from sinks. Kahn's sort has tie-breaks, and viewport "multiplexing" is built in ([dafg_node_scheduling.md](https://github.com/GaijinEntertainment/DagorEngine/blob/main/prog/gameLibs/render/daFrameGraph/dafg_node_scheduling.md)).
  - Related research ([Sandu & Shcherbakov, WSCG 2024](http://wscg.zcu.cz/WSCG2024/JWSCG-2024/B43-2024.pdf)) frames render-pass merging as an NP-complete problem (MLGP). Their greedy solution gives "30% less render pass breaks" than a Granite-like baseline. They report that the Granite approach was "less optimal than the manual approach" on their mobile title, which "uses manual Vulkan render pass placement".
- **Unity URP RenderGraph** (source-available; an industry reference for tilers).
  - Raster passes that write and then read the framebuffer through `SetInputAttachment` are merged into one native render pass. `AddUnsafePass`/`AddBlitPass` block merging because the graph cannot see inside them ([Unity manual](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-framebuffer-fetch.html), [optimize](https://docs.unity.cn/6000.0/Documentation/Manual/urp/render-graph-optimize.html)).
  - The Render Graph Viewer shows merged passes, a section that "Displays the reasons why URP could not merge this render pass with the next render pass", and per resource whether it is **Memoryless** ([viewer reference](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-viewer-reference.html)).

---

## 10. Sebastian Aaltonen — "No Graphics API" (Dec 2025)

Source: [blog post](https://www.sebastianaaltonen.com/blog/no-graphics-api). The prototype API is about 150 lines, "fits in one screen".

- **Barriers without resource lists.** `gpuBarrier(cb, STAGE_COMPUTE, STAGE_COMPUTE)`, plus a hazard bitfield for the few non-coherent caches (`HAZARD_DESCRIPTORS`; ROP flush is implied by a raster-output producer stage; indirect args are implied by the stage).
  - "Users only describe the queue execution dependencies: producer and consumer stage masks. There's no need to track the individual texture and buffer resource states."
  - "the barrier command executed by the GPU contains no information about textures or buffers at all. The resource list is consumed solely by the driver."
  - "Metal 2 has a modern barrier design already: it doesn't use resource lists."
- **Why this works now.** RDNA-class coherent L2 and in-cache (de)compression mean "Layout transitions are no longer needed". He points to VK_KHR_unified_image_layouts (2025) and asks why the resource list is still required. The same point comes from Khronos: "developers no longer need to use layout transitions at all, just use GENERAL", with UNDEFINED (init) and PRESENT_SRC still needed ([Khronos blog, 2025-06-26](https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation)).
- **Split barriers** become timeline-like signal/wait on a GPU pointer: `gpuSignalAfter(..., STAGE_RASTER_COLOR_OUT, ptr, counter, SIGNAL_ATOMIC_MAX)` / `gpuWaitBefore(...)`. Event→wait "see[s] barely any use" today because normal barriers are already complicated.
- **Render passes stay.** "The render pass abstraction doesn't add notable API complexity". `gpuBeginRenderPass(cb, {color/depth targets with loadOp/storeOp})`, and "Render pass begin/end commands don't automatically emit barriers … crucial for efficient depth prepass implementations".
- **Subpasses are called a misstep.** "Subpasses ended up being a high level concept inside a low level API … Vulkan added all of this complexity simply to avoid exposing the framebuffer fetch intrinsic". He prefers framebuffer fetch on mobile and a traditional multipass on desktop: "Apple's Metal examples do the same".
- **Tile memory.** Scratchpads and tile memory "are managed automatically by the driver … Tile memories are stored automatically by the tile rasterizer (store op == store)". Load/store ops are the user's lever.
- **Other points.** Bindless descriptor heap plus a 64-bit root pointer per dispatch; one-shot command buffers per frame; timeline semaphores for frames in flight.
- **Relevance.** Metal 4 already works this way: "Metal 4 puts you in charge of synchronizing your resources". Barriers filter by stages (`barrierAfterEncoderStages:beforeEncoderStages:`, queue barriers across encoders) ([WWDC25 "Explore Metal 4 games"](https://developer.apple.com/videos/play/wwdc2025/254/)). A graph that emits per-boundary *stage* barriers therefore maps 1:1 to Metal 4 and to Vulkan global memory barriers under unified layouts.

---

## 11. Arguments against heavy render graphs, and barrier-only alternatives

- **"A rebuttal of render graphs"** (Jotun Studios blog, 2021; [original](https://blog.jotunstudios.com/a-rebuttal-of-render-graphs/), [archive](http://web.archive.org/web/20210815223109/https://blog.jotunstudios.com/a-rebuttal-of-render-graphs/)).
  - "render graphs have been oversold, especially to hobbyist and indie developers".
  - "Passes in a render graph have implicit dependencies… I'd much rather simply say 'pass A uses the output of pass B'".
  - "you end up having to supply all the information you'd supply in a barrier, except you do it when you create your renderpass… it's less work to just manually issue barriers".
  - It keeps "self-contained passes" as "the most useful part of render graph literature".
  - *Merit:* the duplication cost is real if declarations aren't reused for binding, load/store and usage flags. kajiya, Daxa and skaarj reuse them, which removes the argument. The implicit-dependency point is answered by typed handles that passes *return* (Filament and vuk), so data flow shows in the code.
- **Blade study** (Dzmitry Malyshau, arXiv, July 2026; [abs](https://arxiv.org/abs/2607.26506), [html](https://arxiv.org/html/2607.26506)).
  - Blade keeps images in GENERAL, tracks **no** per-resource state, and places **global barriers at pass boundaries**. The barrier scope comes from pass *kind*: "an encoder always knows what kind of pass it just closed and what kind it is about to open".
  - Removing 15 redundant barriers from 16 independent compute passes cut GPU span 29.3% (RTX 5070) and 32.3% (RX 7900 XT). Render results were mixed, and there was a regression on a Radeon 780M iGPU at high pass counts.
  - wgpu's per-resource tracking cost "between 1.3× and 5.9× the host time".
  - The recommendation is compositional: "the graph can choose the dependency boundaries while the RHI declines to rediscover them".
  - The paper says what it doesn't cover: "no mobile tiler is represented … a barrier that breaks a render pass on a tile-based GPU forces a tile flush and reload — so the class where this design has the most to lose is the class the study says nothing about."
- **Conventions instead of tracking.** HypeHype's "always sampled except inside the render pass" rule ([slides](https://advances.realtimerendering.com/s2023/AaltonenHypeHypeAdvances2023.pdf)); Godot's automatic DAG hidden inside the RHI ([Godot](https://godotengine.org/article/rendering-acyclic-graph/)); Diligent's automatic tracking, which is single-threaded only ([gamedev.net](https://www.gamedev.net/articles/programming/graphics/a-practical-approach-to-managing-resource-states-in-vulkan-and-direct3d12-r5027/)).
- **Merits, summarized.** For a small renderer, barrier derivation is the *least* valuable thing a graph does, and it keeps getting cheaper at the API level (unified layouts, Metal 4 stage barriers). The things only a whole-frame view can do are:
  - lifetimes, so you get transients, memoryless targets and aliasing;
  - correct load/store/discard;
  - culling;
  - keeping tiler render passes unbroken.
- **Tilers specifically.** The costly events are render-pass breaks and wrong load/store ([Vulkan TBR best practices](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)): "use VK_ATTACHMENT_STORE_OP_DONT_CARE for any attachment you don't need", transient + LAZILY_ALLOCATED for pass-local data, and on-tile MSAA resolve.
  - VK_KHR_dynamic_rendering_local_read now gives subpass-style tile-local reads with dynamic rendering, "making dynamic rendering a fully fledged replacement for renderpasses on all implementations, including tile based architectures" ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html), [Khronos "Streamlining Subpasses"](https://www.khronos.org/blog/streamlining-subpasses)).
  - On Apple GPUs, `MTLStorageModeMemoryless` render targets "exist only transiently in on-GPU tile memory" ([Apple docs](https://developer.apple.com/documentation/metal/mtlstoragemode/mtlstoragemodememoryless)).

---

## 12. Comparison table

| System | Dependency declaration | Order | Barrier derivation | Tile / subpass | Transients & aliasing | Temporal | MT recording | Visualization | Size (lines) |
|---|---|---|---|---|---|---|---|---|---|
| **Granite** | String names; `add_color_output/attachment_input/texture_input/history_input`; RMW via `input` arg | Bottom-up traversal + greedy reorder (merge-first, max overlap) | Per-pass invalidate/flush lists, runtime state, lookahead batching, sync2 | **Automatic** greedy subpass merge (`should_merge`), BY_REGION deps | Transient = single physical pass → LAZILY_ALLOCATED; same-desc image aliasing | `add_history_input`, image swap per frame | Serial CPU timeline + parallel per-physical-pass command buffers | `log()` | ~4,970 |
| **Filament FG** | Typed versioned handles; `read/write/sample/declareRenderPass`; Blackboard | Declaration order, refcount cull | **None in graph** (backend tracks layouts) | Derived discardStart/End/clear/readOnly; explicit 2-subpass via `subpassMask` | Texture cache pool by descriptor; no in-frame aliasing | Outside graph (import) | Serial into driver command stream (driver thread) | graphviz + fgviewer web app | ~4,900 |
| **kajiya** | `pass.read/write(handle, AccessType)`; SimpleRenderPass binds in declaration order | Declaration order | Just-in-time `vk_sync` barrier from last access | None | Transient cache by desc; no aliasing | **`get_or_create_temporal(key)`** import/export with access state | Single CB | debug-pass hook | ~4,000 |
| **vuk** | Typed pass params `VUK_IA(eColorWrite)`, `Value<T>` futures, SSA IR | IR scheduling + queue inference | Lowered sync2 barriers per use | README claims; current backend 1 subpass/pass | Allocator-managed; inference of extent/format | Via persistent values | Not found | IR dumper | ~7,600 (core) |
| **Daxa** | `.reads/.writes/.samples`, task heads (shader-shared macros) | Compile once; ASAP batches; optional reorder | One barrier set per batch; no subresource tracking | None | Optional `alias_transients` | `PERSISTENT_DOUBLE_BUFFER` | No | ImGui/ImPlot resource viewer | ~9,700 (incl. UI) |
| **Falcor** | Named fields via `reflect()`; Python `addEdge` | Topological | RenderContext state tracking | None | None (each field allocated) | `Persistent` field flag | No | **Node editor + live edit** | ~5,800 (incl. UI) |
| **Bevy (≤0.18)** | Nodes/edges/slots, no resource decls | Graph order | wgpu | None | None | n/a | Limited | — | removed in 0.19 |
| **skaarj FrameGraph** | Frostbite-style create/read/write + opaque flags | Declaration order, cull | User `preRead/preWrite` hooks | User | Create at first / destroy after last use | User | No | graphviz + web viewer | ~1,550 |
| **Godot 4.3 RD** | Implicit from immediate API | Level sort, type grouping | Automatic, batched per level | Draw list = node | n/a | n/a | Secondary CBs disabled | — | inside RHI |
| **daFrameGraph** | create/modify/read/rename, `historyFor` | Kahn + tie-breaks, prune from sinks | Automatic | Render-pass merging (Sandu) | Yes | **`historyFor`, no edges** | — | — | ~2 MB |
| **Liam Tyler / Deep Spark MVP** | read/write/versioning | Declaration (Liam) / Kahn (DS) | Last-state comparison | None | Aliasing via packing / free-list | — | No | — | ~350–500 (DS) |

---

## 13. Lessons for a lean design (Vulkan + Metal, Apple + Adreno)

### Steal

1. **One typed access list per pass is the single source of truth.** Use a small enum such as `ColorWrite`, `ColorReadWrite`, `DepthWrite`, `DepthRead`, `InputAttachment`, `Sampled(stage)`, `StorageRead/Write(stage)`, `TransferSrc/Dst`, `Indirect`, `Present`. From it derive:
   - (a) barriers;
   - (b) load/store/discard (Filament's rule: no prior writer → DONT_CARE/CLEAR, no later reader → DONT_CARE store);
   - (c) creation usage flags, as the union of all accesses (Filament);
   - (d) transient/memoryless eligibility (Granite);
   - (e) optionally, descriptor binding order (kajiya, Daxa heads).

   Reusing the list this way is what answers the "you write everything twice" rebuttal ([Jotun](https://blog.jotunstudios.com/a-rebuttal-of-render-graphs/)).
2. **Typed, versioned handles that passes return.** `auto hdr = lighting(fg, gbuf);` makes the data flow explicit and greppable (Filament, vuk), avoids Granite and Falcor's string keys, and addresses the "implicit dependencies" complaint.
3. **Declaration order plus refcount culling, no reordering.** Filament's cull is about 40 lines. Reordering is where Granite's and Daxa's complexity and heuristics live, and Daxa admits it conflicts with aliasing. At our pass counts the gain is small ([Liam Tyler](https://liamtyler.github.io/posts/task_graph/)).
4. **Rebuild every frame into an arena.** Filament, kajiya and the Deep Spark MVP all do this. It keeps conditional passes trivial: just don't add them. Precompiled graphs (Daxa) buy CPU time you won't need at this scale. Cache the structure by hash later only if profiling says so.
5. **Tiler merging: explicit opt-in first, conservative automatic grouping second.**
   - The primitive is a "render pass group": consecutive raster passes with identical attachment sets, where each later pass reads earlier outputs only through `InputAttachment` (pixel-local).
   - The graph checks Granite's refusal rules: no non-local sample of a group-produced image, same depth, same queue, no compute in between. It then lowers the group to:
     - **Vulkan:** one dynamic-rendering instance using `VK_KHR_dynamic_rendering_local_read`, which avoids VkRenderPass/subpass objects entirely ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html)).
     - **Metal:** one render command encoder, with framebuffer fetch (`[[color(n)]]`) for local reads.
   - Attachments whose whole lifetime is inside the group become `TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED` on Vulkan, or `MTLStorageModeMemoryless` on Apple ([Apple](https://developer.apple.com/documentation/metal/mtlstoragemode/mtlstoragemodememoryless)).
   - The pass author declares intent (`InputAttachment`) as in Filament and Unity. The graph only validates and groups; it does not search. That sidesteps the NP-hard ordering problem ([Sandu](http://wscg.zcu.cz/WSCG2024/JWSCG-2024/B43-2024.pdf)), and it is roughly Granite's `should_merge` minus the scoring.
6. **Barriers: coarse and batched.** At each pass (or group) boundary, emit **one** `vkCmdPipelineBarrier2` built from the union of `(prev access → next access)` over the resources that changed.
   - Where `VK_KHR_unified_image_layouts` is available, most of that collapses into a single global memory barrier plus UNDEFINED/PRESENT transitions ([Khronos](https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation), [Blade](https://arxiv.org/abs/2607.26506)).
   - The same stage pair maps directly to Metal 4 `barrierAfter...Stages` ([WWDC25](https://developer.apple.com/videos/play/wwdc2025/254/)); Metal 3 automatic hazard tracking needs nothing.
   - Keep the stages precise: raster-out → fragment, not ALL_GRAPHICS → ALL_GRAPHICS. That lets binning overlap on tilers ([TBR guide](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html), [Aaltonen](https://www.sebastianaaltonen.com/blog/no-graphics-api)).
   - kajiya's "skip if same access" rule is enough for WAR/RAR elision.
7. **History as keyed persistent resources.**
   - Use `fg.history("taa", desc)`, returning `(current_write, previous_read)`, ping-ponged and imported with the last-known access state (kajiya `get_or_create_temporal`, Daxa `PERSISTENT_DOUBLE_BUFFER`, Granite `add_history_input`).
   - A history read adds **no in-frame edge** but keeps its producer alive (daFrameGraph).
   - History images are never transient and never aliased (Granite).
8. **Multithreaded recording in two phases** (Granite).
   - (1) A serial "resolve" step walks the passes and computes barriers, load/store and group boundaries. That is cheap and deterministic.
   - (2) A parallel step records each group/pass into its own primary command buffer on Vulkan, or its own `MTLCommandBuffer` enqueued in order on Metal.
   - (3) Submission happens in order.
   - This avoids secondary command buffers inside render passes, which Godot had to disable for hardware-compatibility reasons ([Godot](https://godotengine.org/article/rendering-acyclic-graph/)).
   - For very heavy single passes, split the *draws* inside a pass later: Vulkan secondaries, or Metal `MTLParallelRenderCommandEncoder`.
9. **Visualization for cheap.** A dot dump (Filament and skaarj, well under 100 lines) that labels each pass with its group, load/store flags (`S/E/C` like Filament) and culled state, and each resource with lifetime, transient/memoryless status and alias slot. Add Unity's best idea: **print why two adjacent passes were not grouped**, for example "reads hdr non-locally" ([Unity viewer](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-viewer-reference.html)).

### Avoid (or defer until measured)

- **Pass reordering and scheduling heuristics** (Granite overlap factor, Daxa batch optimization). The code costs a lot and it fights aliasing and merging.
- **Async compute and multi-queue.** Cross-queue ownership is "brutal" ([Granite](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)). Adreno and Apple gain less from it. If added later, use Daxa-style explicit submit points or Muratov's closest-dependency fences, not automatic discovery.
- **Heap-placement memory aliasing** with aliasing barriers. Start with a descriptor-keyed transient pool (kajiya, Filament). Then do Granite's same-descriptor image aliasing if memory pressure shows up on mobile. Memoryless transients already remove the biggest mobile offenders (depth, MSAA, G-buffer).
- **SSA/futures IRs, inference engines, precompiled graphs, per-subresource tracking** (vuk, Daxa). They are elegant APIs but compiler-sized.
- **Execution-only graphs.** Bevy deleted its own for adding boilerplate over a scheduler ([Bevy 0.19](https://bevy.org/news/bevy-0-19/)).
- **String-keyed resources and scripting/editor layers** (Granite, Falcor). A dot dump plus the existing debug views covers visualization.
- **Emulating Vulkan subpasses as the core abstraction.** Aaltonen calls them a misstep, and Vulkan's local_read and Metal's framebuffer fetch are the modern mapping. Model the *group* in your own IR and lower it per backend.

### Minimal API shape implied by the above (sketch)

```cpp
struct GBuf { Tex albedo, normal, depth; };
GBuf gbuffer(Graph& g, Extent2D e) {
    GBuf o;
    g.addPass("gbuffer", [&](PassBuilder& b) {
        o.albedo = b.write(b.create("albedo", {e, RGBA8}), Access::ColorWrite);
        o.normal = b.write(b.create("normal", {e, RGB10A2}), Access::ColorWrite);
        o.depth  = b.write(b.create("depth",  {e, D32}),     Access::DepthWrite);
        return [=](PassContext& c) { /* draws */ };
    });
    return o;
}
Tex lighting(Graph& g, GBuf in) {
    Tex hdr;
    g.addPass("lighting", [&](PassBuilder& b) {
        b.read(in.albedo, Access::InputAttachment);   // pixel-local -> same group, memoryless-eligible
        b.read(in.normal, Access::InputAttachment);
        b.read(in.depth,  Access::DepthRead);
        auto [cur, prev] = b.history("hdr", {in.albedo.extent(), RGBA16F});
        b.read(prev, Access::Sampled(Stage::Fragment));
        hdr = b.write(cur, Access::ColorWrite);
        return [=](PassContext& c) { /* fullscreen */ };
    });
    return hdr;
}
// compile(): cull -> groups (validate InputAttachment locality) -> lifetimes (transient/memoryless)
//            -> per-boundary barriers + load/store; execute(): parallel record per group, ordered submit.
```

Expected size, going by the reference points above: ~500–1,000 lines for the core, meaning handles, builder, cull, grouping, lifetimes, barrier and load/store derivation, and a dot dump. Backend lowering is extra. The Deep Spark MVP (~350–500 lines with aliasing) and skaarj (~1,550) bracket this; Granite (~5k) and Filament (~4.9k) are the ceiling to stay well below.
