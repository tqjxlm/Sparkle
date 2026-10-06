# Render Graph

Every renderer records its frame through a render graph. Each GPU pass declares what it reads and writes; the graph derives everything a renderer used to write by hand: which passes run, image layouts and barriers, attachment load/store actions, image usage flags, which raster passes share one hardware render pass, which reads stay in tile memory, and which transients need no memory at all. Every decision is recorded with its reason and can be dumped, viewed and diffed in tests.

## Overview

The graph serves three goals, in priority order:

1. **Explicit dependencies.** The frame is a data structure: passes and the resources they access, in the order they run. It can be dumped to JSON, rendered as a page, and gated by golden tests.
2. **Derived synchronization.** No renderer code transitions an image or picks a load op. Barriers, layouts, load/store actions and usage flags come from one access list per pass, which also validates the shader bindings.
3. **On-chip data.** Consecutive raster passes merge into one render pass, a pass may read what an earlier one wrote at the same pixel without leaving tile memory, and attachments that never leave their render pass are memoryless. Where passes do not merge, the graph says why.

The model is deliberately small:

* **Rebuilt every frame.** A graph is built, compiled, executed and destroyed within one frame. Conditional features are plain `if`s; a frame has 5–20 coarse passes, so compiling costs microseconds.
* **Declaration order is execution order.** The graph never reorders passes, so every access depends on the last write before it in source order, handles are plain indices, and a capture reads like the code.
* **The graph owns render passes.** A raster pass draws into attachments the graph begins and ends; it cannot change them. This is what makes merging and load/store inference possible.
* **Every optimization has a kill switch** (see [Knobs](#knobs)), and every decision a reason in the dump.
* **Single-threaded recording.** The render thread records the whole frame into one command buffer on one queue.

A frame on the render thread: `Renderer::Tick` updates host-side data (and resizes persistent resources when the resolution changed), then `Renderer::Render` creates the graph, lets the renderer add its scene passes, adds the shared post chain, compiles (pure CPU, no RHI calls), executes, and returns the dump when one was requested.

## Structure

| Area | Entry points | Role |
| --- | --- | --- |
| Graph core | [RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h) (`RenderGraph`, `RGBuilder`, the pass contexts, `RGTexture`/`RGBuffer`/`RGAccelerationStructure`) | building, compiling, executing and dumping one frame |
| Transient images | [RGTexturePool.h](../libraries/include/renderer/graph/RGTexturePool.h) | images behind transients, kept across frames and renderer recreation; `RenderFramework` owns it |
| Pass timing | [RGPassTimers.h](../libraries/include/renderer/graph/RGPassTimers.h) | GPU timers per physical pass, kept across frames by the renderer |
| Errors | [RGError.h](../libraries/include/renderer/graph/RGError.h) | always-on checks; `RGErrorsThrow` makes them throw under test |
| Renderers | [Renderer.h](../libraries/include/renderer/renderer/Renderer.h), [RasterRenderer.h](../libraries/include/renderer/renderer/RasterRenderer.h), [PostChain.h](../libraries/include/renderer/pass/PostChain.h) | the frame template, the shared Forward/Deferred base and the post chain every renderer ends with |
| Passes | [libraries/include/renderer/pass/](../libraries/include/renderer/pass/), [ColorSlot.h](../libraries/include/renderer/pass/ColorSlot.h) | pass classes keep pipelines and shaders and add their pass through `AddTo(graph, inputs...)` |
| RHI lowering | [RHIRenderingInfo.h](../libraries/include/rhi/RHIRenderingInfo.h), [RHICommandContext.h](../libraries/include/rhi/RHICommandContext.h), [RHIBarrier.h](../libraries/include/rhi/RHIBarrier.h), [RHIPass.h](../libraries/include/rhi/RHIPass.h) | dynamic rendering, access-based barriers, pipelines per attachment signature, pass timestamps; the RHI only lowers what the graph planned |
| Tools | [dev/render_graph_viewer.py](../dev/render_graph_viewer.py), [dev/apple_gpu_traffic.py](../dev/apple_gpu_traffic.py) | offline graph pages; GPU memory traffic on Apple GPUs |
| Tests | [tests/render_graph/](../tests/render_graph/) | synthetic graphs, error cases, merge parity, golden graph shapes |

The RHI knows nothing about graphs: it exposes `RHICommandContext::BeginRendering` over an `RHIRenderingInfo`, barrier batches, and the device queries the compiler and passes ask (`SupportsPixelLocalRead`, `SupportsMemorylessImage`, `GetTileBudget`, `SupportsLinearFiltering`). RHI tests in [tests/rhi/](../tests/rhi/) use these directly.

## Writing Passes

A pass class keeps its persistent state and adds one pass per frame. The setup lambda declares accesses on the builder and returns the lambda that records:

```cpp
void SkyBoxPass::AddTo(RenderGraph &graph, RGTexture sky_map, const RHISampler::SamplerAttribute &sky_map_sampler,
                       RGTexture scene_color, RGTexture scene_depth) const
{
    graph.AddRasterPass("SkyBox", [this, sky_map, sky_map_sampler, scene_color, scene_depth](RGBuilder &builder) {
        using Table = SkyBoxPixelShader::ResourceTable;
        builder.Sampled(sky_map, &Table::sky_map, &Table::sky_map_sampler, sky_map_sampler);
        builder.ColorWrite(scene_color, ColorSlot::SceneColor);
        builder.DepthTest(scene_depth);
        return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
    });
}
```

A renderer composes these in `BuildGraph(graph)`, which returns the texture the scene ends in; `Renderer::Render` adds the post chain and runs the graph. Outside a renderer (tests), the owner calls `Compile()` and `Execute(command_context)` itself and destroys each graph before its texture pool.

### Resources

* **Transients** (`CreateTexture`) live within the frame. Their size is a size class (`Scene`, `Output`, or `Absolute`) resolved against the frame's `RenderResolution`, and their usage flags are the union of the accesses declared on them. A transient is a single-sampled 2D texture with one mip.
* **Imports** (`Import`) bring in anything persistent: history and accumulators, IBL and sky maps, the back buffer, staging buffers, the TLAS. Every buffer and acceleration structure is an import. Importing one image twice returns the first texture, so passes sampling a shared image need not coordinate.
* **Subresources.** An access covers the whole texture, or one mip (`texture.Mip(m)`) or one subresource (`texture.Subresource(m, layer)`), e.g. one cube face.
* `ReadOnHost(buffer)` marks a buffer the host reads after the frame (screenshot readback).

### Pass Kinds

| Kind | Added with | Its context records |
| --- | --- | --- |
| Raster | `AddRasterPass` | draws inside the rendering of its physical pass; with `NativeAccess()`, raw commands of foreign code (ImGui) |
| Compute | `AddComputePass` | dispatches, inside the given `RHIComputePass` |
| Copy | `AddCopyPass` | copies, and acceleration structure builds |
| External | `AddExternalPass` | anything, through the raw command context (the denoisers) |

Accesses: `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `PixelLocalRead(slot)`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst` on textures; `CopySrc`, `CopyDst` on buffers; `AccelerationStructureBuild` and `AccelerationStructureRead` on acceleration structures. Pass flags: `FullyOverwrites()` (every written texel is written, so the old contents can be discarded), `SideEffect()` (never culled), `NativeAccess()`.

Rules every pass follows:

* **Declare and bind.** A shader access names the `ResourceTable` member that reads the resource, and the graph binds it into every pipeline of the pass that has the member. Each input is written once.
* **Consumers own samplers.** Images carry no sampler; a `Sampled` access names the sampler binding and attributes the pass samples with. Samplers several passes share live on the owner of the resource (`SkyRenderProxy::SkyMapSampler`, `ImageBasedLighting::MapSampler`).
* **Declare everything bound.** A pipeline keeps bindings from earlier passes and frames, so a pass must declare every graph resource bound in the pipelines it draws or dispatches; the graph checks it. `SampledOrPlaceholder` binds a placeholder outside the graph for an optional input (no directional light, an IBL map still cooking).
* **Record only through the context.** `RHIContext::GetCommandContext()` is for code outside graph passes and asserts while a graph executes. A pass leaves each declared image in its declared state.
* **Fixed color slots.** Each attachment role has one slot in every pass ([ColorSlot.h](../libraries/include/renderer/pass/ColorSlot.h)): Screen and BackBuffer 0, SceneColor 1, GBufferPacked 2, DepthCopy 3; others use 0. A pixel shader writes a role through the output structs of [color_slot.h.slang](../shaders/include/color_slot.h.slang).

### Reading Pixel-Locally

A pass that reads what an earlier pass wrote at the same pixel declares `PixelLocalRead(texture, slot, ...)` instead of `Sampled`. It is a request: the compiler keeps it pixel-local when the writer shares the reader's physical pass, and otherwise lowers it to a sampled read, with the reason in the dump. The pass ships two shader variants sharing one `ResourceTable`, and its record function draws with the one `context.IsPixelLocal(texture)` selects. `ToneMappingPass` (sampled base variant) and `DirectionalLightingPass` (loaded base variant) are the examples.

* A variant is a `<source>:<DEFINE>` entry of `SHADER_VARIANTS` in [shaders/CMakeLists.txt](../shaders/CMakeLists.txt), loaded with `RHIContext::CreateShader<T>("<DEFINE>")`.
* In the variant, the input is a global `SubpassInput` with `[[vk::input_attachment_index(slot)]]` under the sampled texture's name and binding, read with `SubpassLoad()`. Vulkan reads it as an input attachment, Metal as a framebuffer fetch (`[[color(slot)]]`).
* The variant pipeline compiles only once the graph keeps a read pixel-local, so a device without pixel-local reads never compiles a shader it cannot run.

## What the Compiler Decides

`Compile()` validates the declarations and plans the frame. Each decision below appears in the [dump](#getting-a-dump) with a reason field.

* **Culling.** A pass runs if it has a side effect, writes an import, or writes something a later live pass uses. Disconnecting a debug view therefore removes everything that only fed it. Culled passes stay in the dump with `cull_reason`.
* **Physical passes.** Consecutive live raster passes merge into one rendering over the union of their attachments until the next pass breaks a rule: another pass kind, a different size or depth attachment, a slot conflict, a clear inside the pass, a read that would need a barrier inside the rendering (`NonLocalRead`), attaching something already read (`AttachmentReadInPass`), a pixel-local read the device cannot do, or merging disabled. The dump lists every rule each boundary breaks (`breaks`) and the first one (`break_reason`).
* **Pixel-local reads.** A request stays pixel-local where its writer attaches the texture at that slot in the same physical pass; otherwise it is lowered to `Sampled` with `lowered_reason`.
* **Memoryless.** A transient used only as an attachment or pixel-local input within one raster physical pass never leaves tile memory. It is memoryless in the plan on every device; the dump's `backing` says whether the device gave it memoryless storage or an ordinary image.
* **Aliasing and pooling.** Transients with disjoint lifetimes and matching format, size and memorylessness share an image, assigned deterministically, so the same graph maps to the same images every frame. Images come from the `RGTexturePool` and are reused by the very next frame.
* **Barriers.** Planned per subresource from each image's tracked state, so the first access of a frame waits for the previous frame's last one, and the swap chain image's first write waits for its acquire. Barriers are batched before each physical pass; a pixel-local read gets a by-region barrier inside the rendering. Metal records none (see [Caveats](#caveats-and-limitations)).
* **Load/store.** `Clear` when the access clears, `DontCare` when nothing earlier wrote the texture or the pass fully overwrites it, otherwise `Load`; `Store` when a later pass uses the contents or the texture is imported, otherwise `DontCare`.
* **Tile budget.** Each raster physical pass reports its color bytes per pixel against the device's budget (Apple GPUs by family, Mali 32 B, unknown elsewhere). Exceeding it is a warning: tilers shrink tiles rather than spill.
* **Opportunity report.** One line per physical pass, the most bytes moved first, naming the rules that kept the next pass out and what each load and store costs. It tells authors which declaration to change.

`Execute()` records each physical pass with its barriers, members and bindings, writes the planned states back to the images' trackers (so foreign code and the next frame see them), and checks each pass's contract.

## Renderers

`RenderFramework` owns the texture pool, so a pipeline switch reuses images both pipelines need. Only a pipeline switch recreates the renderer. A resolution change (`width`, `height`, `render_scale`, or a window resize, which sets the output to the window size) resizes it in place: transients re-resolve every frame, and the renderer's `OnResize` hook reallocates its persistent imports (the GPU accumulator and denoisers, the CPU host buffers).

Every frame ends with the post chain (`PostChain`): the screen pass (`ToneMapping`, or `Upsample` for the CPU renderer) into the Screen transient at output resolution, an optional screenshot readback, `Ui` (ImGui, drawn in the screen pass's physical pass), and `Present` into the back buffer with the window's pre-rotation. `GraphView` replaces the screen pass while `render_graph_view` shows a texture.

* **Forward:** `DirectionalShadow` | `BasePass` + `SkyBox` + `ToneMapping` + `Ui` | `Present`, with SceneColor and SceneDepth memoryless.
* **Deferred:** `DirectionalShadow` | `GBuffer` + `Lighting` + `SkyBox` + `ToneMapping` + `Ui` | `Present`. Lighting reads the GBuffer and a depth copy pixel-locally (Metal cannot fetch depth, so `GBuffer` also writes the device depth to an `R32Float` DepthCopy slot), and GBufferPacked, DepthCopy, SceneDepth and SceneColor are memoryless: 32 B of color per pixel plus depth.
* **GPU:** `ClearAccumulator` when the camera moved or the scene changed, `BuildTLAS` (Copy), `PathTrace` (Compute), one External pass per denoiser ([Denoiser.md](Denoiser.md)), then the post chain.
* **CPU:** an `Upload` Copy pass from the host-tone-mapped buffer into SceneColor, then the post chain.
* **IBL cook:** while a map cooks on the GPU, `ImageBasedLighting::AddCookPasses` starts the Forward or Deferred graph with one cook step per map; `IblCookAccelerator` runs steps in graphs of their own. The GPU and CPU renderers never sample the IBL, so no map cooks under them; the first Forward or Deferred frame cooks it.

Forward and Deferred share `RasterRenderer`, which adds the IBL cook, the shadow map, the lighting inputs and the sky box around each renderer's `AddScenePasses`. At `render_scale` < 1 tone mapping resamples, so the main physical pass ends before it (`TargetSizeMismatch`); without pixel-local reads it ends at every pixel-local read (`NoPixelLocalSupport`). Both render the same pixels.

The exact frame of each renderer is in its golden ([tests/render_graph/golden/](../tests/render_graph/golden/)); the [viewer](#viewer) shows any frame.

## Debugging

### Knobs

| cvar | default | Effect |
| --- | --- | --- |
| `render_graph_view` | *(empty)* | Shows a graph texture in place of the frame (see [Viewing a Texture](#viewing-a-texture)). |
| `render_graph_cull` | `true` | `false` keeps every pass. |
| `render_graph_merge` | `true` | `false` records every pass as its own physical pass. Same pixels. |
| `render_graph_memoryless` | `true` | `false` backs every transient with an ordinary image. Same pixels. |
| `render_graph_pixel_local` | `true` | `false` lowers every pixel-local read to a sampled one, as on a device without support. Same pixels. |
| `render_graph_full_barriers` | `false` | Adds an all-commands barrier before every physical pass (Vulkan only). A bug that disappears with it lies in a planned barrier. |
| `render_graph_tile_budget` | `0` | Overrides the device's tile budget in bytes per pixel (`0` = the device's). |
| `render_graph_tile_budget_split` | `false` | Breaks physical passes that exceed the tile budget instead of warning, for on-device A/B measurements. |
| `render_graph_export` | `false` | Exports the graph once the scene is ready (see [Exporting the Graph](#exporting-the-graph)). |
| `render_graph_profile_frames` | `0` | Makes every export aggregate timings over that many frames. |
| `validate_sync` | `false` | Vulkan synchronization validation, which reports hazards no barrier orders; needs `validation` ([Test.md](Test.md#validation-layer)). |

### Viewing a Texture

`render_graph_view` names a graph texture, e.g. `SceneDepth` or `IblBrdf`; the post chain draws it into Screen instead of the scene, on every renderer, and culling removes every pass that only fed the scene. The texture must be sampleable as a float 2D texture (`RenderGraph::CanSample2D`); otherwise the frame shows as usual and a warning is logged.

### Exporting the Graph

* `Save Graph Dump` on the control panel's screenshot page writes the next graph to `screenshots/<scene>_<pipeline>_<time>.json` under the [external storage path](Run.md#external-storage-paths).
* `render_graph_export` writes `screenshots/render_graph_export.json` once per run, for headless, CI and device runs (Android takes it from the config file).
* With `render_graph_profile_frames` N, an export waits for N consecutive frames of one graph structure and adds the mean, min and max of each pass's CPU time and each physical pass's GPU time.

### Getting a Dump

`RenderFramework::RequestGraphDump(name)` writes the next executed graph to `screenshots/<name>.json`; a screenshot request with `dump_graph` dumps the screenshot's own frame. From the command line:

```bash
python3 run.py --framework glfw --test_case render_graph_dump --headless true --pipeline deferred
python3 dev/render_graph_viewer.py <external-storage-path>/screenshots/render_graph.json
```

The dump holds passes (accesses, barriers, cull reasons, CPU time), physical passes (members, attachments with load/store reasons, breaks, GPU time), resources (kind, size class, image, lifetime, memoryless and backing), totals (render passes, barriers, bytes loaded, stored, transient and memoryless) and the opportunity report. It names size classes, not pixel sizes, so a steady frame dumps the same structure at any resolution and on either backend.

### Viewer

`python3 dev/render_graph_viewer.py <dump.json>` writes one self-contained HTML page that works offline. It inlines [elkjs](https://github.com/kieler/elkjs) from the `thirdparty/elkjs` submodule (`git submodule update --init --depth 1 thirdparty/elkjs`). Three tabs:

* **Graph:** passes laid out top to bottom, physical passes as boxes with their break reasons and GPU time, edges labelled with the resources between two passes, their accesses, load/store and barriers. Clicking a node highlights what it depends on and what depends on it. Unread stores, memoryless transients and imports are color-coded.
* **Lifetimes:** one bar per resource from its first to its last use, with the image it aliases.
* **Grid:** a pass × resource table with physical-pass bands, followed by the opportunity report.

### Pass Timing

Raster physical passes are timed per physical pass (named by their members joined with `+`, e.g. `BasePass+SkyBox`), compute passes by their `RHIComputePass`; Copy and External passes are untimed. A time arrives `max_frames_in_flight` frames after its pass ran, so timers live in the renderer, not the graph. Timing needs `RHIContext::SupportsPassTimestamps()`.

### Errors

Declaration and recording mistakes log a message and abort in every build, Release included (where `ASSERT` compiles out): invalid handles, accesses a pass kind may not declare, conflicting attachments, transients read before written, imports missing usages, undeclared resources reached or bound, a pass leaving an image in another state, compiling or executing twice, two live graphs on one pool. Under test, `RGErrorsThrow` turns them into `RGError` exceptions; `render_graph_errors` makes one mistake per check.

### Measuring Memory Traffic

* **Apple:** `python3 dev/apple_gpu_traffic.py -- <app arguments>` records the macOS app with `xctrace` and the [Performance Limiters template](../dev/instruments/SparklePerformanceLimiters.tracetemplate), and prints bytes read and written and GPU time per frame per encoder, as a median over runs. Compare configurations, e.g. with and without `--render_graph_merge false`; Apple GPUs compress stored targets, so absolute bytes fall below the dump's `store_bytes`.
* **Android:** Perfetto's `gpu.counters` gives bandwidth on Adreno; `gpu.renderstages` counts render passes, and needs the profileable app and `debug.graphics.gpu.profiler.perfetto=1`.
* Against `render_graph_merge false`, merged Deferred frames write 3.9× fewer bytes per fragment on an S25 Ultra (Adreno 830) and 2.1× fewer bytes per frame on an M5 Max; Forward frames 2.1× and 1.5×.

## Tests

* `render_graph_compile` builds synthetic graphs and checks their plans (culling, aliasing, every break reason, pixel-local lowering, memoryless, barriers, load/store, bindings), executes them and reads the results back. `render_graph_sync_validation` runs it under Vulkan synchronization validation.
* `render_graph_errors` covers every error check.
* `render_graph_merge_parity` (and `_deferred`) switches merging, memoryless and pixel-local reads at runtime and requires bit-identical screenshots; the `_sync_validation` variants run it under synchronization validation.
* `render_graph_pass_timing`, `render_graph_export` (and `_profile`), and `pipeline_switch_pool` cover timing, exports and pool reuse; `forward_render_static_sampled` and `deferred_render_static_sampled` screenshot the sampled fallback.
* `render_graph_ui` is a windowed development case (no registry entry, since headless runs create no ImGui) that checks the UI merges into the screen pass.
* `forward_resize`, `deferred_resize`, `forward_render_scale_change`, `cpu_resize_64spp` and `gpu_resize_64spp` resize at runtime, compare against the static ground truth and require that the renderer is not recreated.
* [tests/build_system/test_render_graph_viewer.py](../tests/build_system/test_render_graph_viewer.py) unit-tests the viewer and the golden projection.

### Golden Graph Shapes

Each renderer has a golden in [tests/render_graph/golden/](../tests/render_graph/golden/), checked by a `<pipeline>_graph_shape` case (plus `forward_graph_shape_no_merge`, `forward_graph_shape_halfres`, `deferred_graph_view_shape` and `deferred_graph_view_fallback`). [graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, physical pass, attachment and resource, without times and byte counts, diffs it against the golden, and renders the dump through the viewer to `screenshots/captures/render_graph_<case>.html`, which CI uploads with the screenshots. An unintended split of a physical pass therefore fails CI.

* Goldens are frames of the default TestScene, run headless (no `Ui`), on the golden cells macos-macos and ubuntu-glfw, which both take the pixel-local path.
* The GPU golden needs a frame that traces, so `gpu_graph_shape` dumps once a frame accumulates with no clear or TLAS build (`render_graph_dump_accumulating`), and needs ray queries (ubuntu-glfw).
* A golden is a shape gate, not a pure function of the code. Besides the passes and their declarations, it changes with the previous frame's graph (first barriers start from the state it left), the pool's creation order, the scene (light, sky map, ready IBL maps), `shadow_map_resolution`, and the resolution (equal-sized transients may share an image).

### Updating a Golden

Run the shape case, whose compare step fails with the diff and leaves the dump behind, then rewrite the golden from it. `dev/run_tests.py` forwards unknown arguments to the app, so `--update` goes to the evaluator directly; each case overwrites the same dump, so update one golden per run.

```bash
python3 dev/run_tests.py --framework glfw --config Release --case deferred_graph_shape
python3 tests/render_graph/graph_shape_test.py --framework glfw --golden deferred --update
```

## Caveats and Limitations

Scope:

* Graph textures are single-sampled; transients are single-mip 2D textures; graph buffers support only copies and host reads (binding one is an error). Imports cover every other case.
* Merging is greedy in declaration order. One non-raster pass between two raster passes splits them; a screenshot without UI does this on purpose (its readback sits between tone mapping and `Ui`).
* External passes (the denoisers) never merge. Images internal to NRD and MetalFX are invisible to the graph and keep their own transitions.
* ImGui leaves its viewport and scissor set, so `Ui` must be the last member of its physical pass; it is, because `Present` attaches the back buffer at Screen's slot.
* Metal 3 tracks hazards automatically: the plan and the tracked states are computed on Metal too, but barrier batches record nothing there.

Platforms:

* Vulkan 1.3 with dynamic rendering, synchronization2 and `independentBlend` (per-slot write masks in merged passes) is required. Every platform compiles against [thirdparty/Vulkan-Headers](../thirdparty/Vulkan-Headers), because the Android NDK's headers predate `VK_KHR_dynamic_rendering_local_read`.
* Pixel-local reads need `VK_KHR_dynamic_rendering_local_read` on Vulkan and an Apple GPU on Metal. The iOS simulator, Intel/AMD Macs and the Android emulator have none and take the sampled path.
* Memoryless storage exists on Apple GPUs (not the iOS simulator) and on Vulkan devices with lazily allocated memory (mobile GPUs, MoltenVK), not on desktop GPUs or lavapipe. MoltenVK backs no input attachment with it, and restarts its Metal render pass at every pixel-local barrier, so transients that cross one get ordinary images there and pixel-local reads save no bandwidth on MoltenVK.
* Apple GPUs keep an unwritten slot's contents even without a write mask, so only Vulkan drivers that write undefined values can catch a missing mask.
* lavapipe loses its blend state after a pipeline without color attachments binds, so the Vulkan backend gives depth-only pipelines and renderings one unused color slot.
* Screen passes sample without anisotropy: some drivers (llvmpipe, Adreno) apply sampler anisotropy even with nearest filtering, which makes 1:1 sampling inexact.
* The macOS runners' paravirtual GPU and MoltenVK's anisotropic sampling occasionally render a frame slightly differently, so the merge-parity cases set `sampler_anisotropy` off and accept a configuration once a later capture repeats an earlier one.

Shaders:

* Slang ignores the `SV_Target` index of a returned value, so role outputs are struct members with an explicit `vk::location`. Check cooked shaders, not source attributes: SPIR-V must decorate the input with `InputAttachmentIndex`, MSL take `[[color(slot)]]`.
* On Slang's Metal target a `SubpassInput` cannot sit in a `ParameterBlock`, a helper reading one needs `[ForceInline]`, and it is fragment-only; `SubpassInput<float>` emits invalid SPIR-V, so DepthCopy is read as a `float4`.

Timing:

* Metal times whole physical passes only; members get debug labels but no times. Passes that share a name share a timer and report its last run. A Metal pass none of whose stages ran reports 0 ms. The first `max_frames_in_flight` frames after a structure change have no times.

ImGui:

* ImGui's Vulkan and Metal backends draw inside a rendering with several color attachments (slot 0 against the physical pass's attachment signature) only after v1.92.9b, so `thirdparty/imgui` pins an upstream master commit until a release contains the change.
* The Vulkan ImGui backend holds one pipeline, which `VulkanUiHandler` recompiles after a device idle when the physical pass's signature changes.

## Opportunities

Found while building the graph; none has a user yet. Each says what would justify it.

Graph features:

* **MSAA:** a `ResolveTo(src, dst)` access lowered to Vulkan resolve attachments and Metal `MultisampleResolve` store actions, with a memoryless MSAA source, once a renderer needs MSAA.
* **History API:** a keyed `graph.History(key, desc) -> {current, previous}` for the first ping-pong user (TAA, temporal AO); today's temporal users update their imports in place.
* **Transient mips, layers and cube maps,** and graph buffers beyond copies (uniform, storage, indirect, transient buffers), Copy passes that blit, clear or generate mips, and indirect dispatch, each with its first user.
* **Heap aliasing** of transients, once dumped transient bytes on mobile justify its aliasing barriers and untracked Metal heaps. Memoryless already removes the largest tiler allocations.
* **Async compute** as a per-pass attribute scheduled from dependency levels, and **parallel recording** by physical-pass spans into separate command buffers submitted in order, once a profile shows the render thread or a queue idle.
* **Metal 4 backend:** lower the existing barrier plan to Metal 4 stage barriers over untracked resources.
* **Compile caching** by graph hash, only if compile time shows up in a profile.
* `graph.Extract`, mandatory bindings, `NativeAccess` on Compute passes and a pool-reuse kill switch were designed and left out until a user exists.

Synchronization and load/store:

* **Read-only depth:** a `DepthTest` attachment synchronizes as a depth write and stores `DontCare`; a distinct `RHIStoreOp::None` lowering to `STORE_OP_NONE` and a read-only depth layout would let it synchronize as a read.
* **Unified image layouts:** collapse layouts to `GENERAL` where `VK_KHR_unified_image_layouts` exists (desktop only; Mali's transaction elimination needs precise layouts), and fold later readers of one write into its first barrier.
* **Foreign writes:** the MetalFX scaler's output carries shader-write usage only so the graph can declare the scaler's write; a builder access for writes outside shaders would remove it.
* **Common tracked state:** Vulkan `Upload`/`UploadFaces` and `EndFrame`'s present transition change tracked image state in backend code, so tracked states differ between backends until the first graph frame; moving them to common code would make more first barriers backend-independent.
* **Tile budget on Adreno:** `VK_QCOM_tile_properties` (exposed on the S25 Ultra) or per-vendor hard limits, once on-device measurements with `render_graph_tile_budget_split` show a split wins.

Tooling:

* The viewer joins passes with pass-to-pass edges, which get dense where one resource feeds many passes; resource-version nodes would scale better.
* Per-pass output thumbnails in the viewer need readback of intermediate images, and memoryless attachments have none to read.
* A public `ImGui_ImplVulkan_CreatePipeline` upstream would allow a per-signature pipeline cache, if signatures ever alternate from frame to frame.
