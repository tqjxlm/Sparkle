# Render Graph

The render graph ([libraries/include/renderer/graph/RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h)) records one frame's GPU passes from the accesses they declare to textures, buffers and acceleration structures. From these declarations it derives which passes run, which images back the transient textures, and the image layouts, barriers and attachment load/store actions, and it can dump every decision with its reason. A graph lives for one frame: it is built, compiled, executed and destroyed, and every renderer records its frame through one.

The RHI only lowers the graph's plan: `RHICommandContext::BeginRendering` over an `RHIRenderingInfo` (attachments already in their attachment layouts, with load/store actions and clear values), access-based barriers, and pipelines compiled per attachment signature. RHI tests ([tests/rhi/](../tests/rhi/)) record through these primitives directly.

## Adding a Pass to a Renderer

A renderer builds its graph in `Render()` from the texture pool it was given, adds its scene passes, ends with the post chain (`Renderer::AddPostChain`, see [Renderers](#renderers)) and hands the graph to `Renderer::ExecuteGraph`, which compiles it, executes it with the renderer's pass timers and dumps it when a dump is pending. A pass class keeps its persistent state (pipelines, shaders, uniform buffers) and adds its pass through an `AddTo(graph, inputs...)` method. The setup lambda declares the pass's accesses on the builder and returns the lambda that records the pass.

```cpp
void MyRenderer::Render()
{
    RenderGraph graph(rhi_, graph_texture_pool_, render_config_);

    const auto history = graph.Import("History", history_image_);
    const auto scene_color = graph.CreateTexture("SceneColor", SceneColorDesc);

    graph.AddRasterPass("Lighting", [this, history, scene_color](RGBuilder &builder) {
        using Table = LightingPixelShader::ResourceTable;
        builder.Sampled(history, &Table::history, &Table::historySampler, HistorySampler);
        builder.ColorWrite(scene_color, 0, Vector4(0, 0, 0, 1));
        return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
    });

    AddPostChain(graph, scene_color, tone_mapping_pass_.get());
    ExecuteGraph(graph);
}
```

Outside a renderer, as in the tests, the owner of a graph calls `Compile()` and `Execute(command_context)` itself, and destroys each graph before its texture pool.

## Reference

### Resources

* `CreateTexture(name, desc)` makes a transient texture: single-sampled, one mip, one layer, sized by its size class (`Scene`, `Output`, or `Absolute` with an explicit width and height), which resolves against `RenderResolution` when the texture is created.
* A transient's image usage is the union of the accesses live passes declare on it.
* `Import(name, image)` brings in a persistent single-sampled image (history, the back buffer, IBL maps). Importing an image again returns the texture, and name, of its first import, so passes that sample the same image need not coordinate.
* `Import(name, buffer)` and `Import(name, tlas)` bring in a persistent buffer (`RGBuffer`: staging and host buffers) or top-level acceleration structure (`RGAccelerationStructure`); importing one again returns its first import. Every buffer and acceleration structure is an import.
* `ReadOnHost(buffer)` marks a buffer the host reads once the graph's commands complete, such as a readback staging buffer.
* A texture access covers every mip and array layer, or the ones `texture.Mip(mip)` (one mip of every layer) or `texture.Subresource(mip, layer)` select, e.g. the face of a cube map a clear writes. A pass may declare disjoint subresources of one texture.
* `RGTexture`, `RGBuffer` and `RGAccelerationStructure` are plain indices. Passes run in declaration order and every access depends on the last write before it in that order, so handles need no versions.
* `FindTexture`, `CanSample2D`, `GetFormat` and `GetSize` answer questions about textures while passes are added; the post chain uses them to find the texture `render_graph_view` names and to size screenshot buffers.

### Accesses

* Textures: `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`.
* Buffers: `CopySrc`, `CopyDst`.
* Acceleration structures: `AccelerationStructureBuild` (a build or refit) and `AccelerationStructureRead` (ray queries).
* An attachment (`ColorWrite`, `DepthWrite`, `DepthTest`) is one subresource. A color slot is the fragment output location.
* Shader accesses take an optional stage mask; the default is the pass kind's stage (Raster: pixel, Compute: compute, External: all).
* A pass declares each texture subresource, buffer and acceleration structure once.

### Pass Flags

* `FullyOverwrites()`: the pass writes every texel of the textures it writes, which discards the previous contents of those it does not also read.
* `SideEffect()`: the pass runs even when nothing reads its outputs.
* `NativeAccess()`: a Raster pass records foreign commands (ImGui) through the raw command context (`GetNativeContext()`), inside the rendering the graph begins over its attachments. The foreign code resets the backend state it records around.

### Bindings

* A shader access may name the binding member of a shader's `ResourceTable` that reads the resource, e.g. `&ToneMappingPixelShader::ResourceTable::screenTexture`: a `Texture2D` member for `Sampled`, a `StorageImage2D` member for the storage accesses, an `AccelerationStructure` member for `AccelerationStructureRead`.
* While the pass records, every pipeline drawn or dispatched through the command context binds the resource there, in each of its resource tables that has the member, so each input is declared once and serves every pipeline of the pass that uses the table.
* A sampled binding binds the image's default view of every subresource. A storage binding binds a view of its access's single mip, with a cube's layers as a 2D array. Views are created at compile.
* A `Sampled` access may also name a `Sampler` member and the attributes of the sampler the pass samples with, e.g. `&ToneMappingPixelShader::ResourceTable::screenTextureSampler`; the graph binds that sampler from `RHIContext::GetSampler`'s cache. Images carry no sampler: the pass that samples a texture decides how.
* Samplers that several passes share live on the class that owns the sampled resource: `SkyRenderProxy::SkyMapSampler` for the sky map (the sky box, the IBL cooks and the GPU path tracer) and `ImageBasedLighting::MapSampler` for the IBL maps.
* `SampledOrPlaceholder` declares an input that may be missing: an invalid texture binds the given placeholder image, which is outside the graph, with the given sampler, so no pipeline keeps an image an earlier graph bound.
* Every binding must reach at least one pipeline the pass draws or dispatches, unless the pass draws or dispatches nothing (an empty scene).
* Uniform buffers and other resources outside the graph stay bound by the pass itself.
* A pipeline keeps what earlier passes and frames bound, so a Raster or Compute pass must declare every graph resource bound in the pipelines it draws or dispatches, whoever bound it.

### Pass Kinds

Each kind's record function receives a context that exposes only what the kind may record:

| Kind | Added with | Context records | The graph around it |
| --- | --- | --- | --- |
| Raster | `AddRasterPass` | draws; with `NativeAccess()`, anything the open rendering allows | begins and ends rendering over the declared attachments; the opening barriers sit inside the pass's debug label and timer |
| Compute | `AddComputePass`, with an `RHIComputePass` | dispatches | brackets the pass with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | `AddCopyPass` | copies between images and buffers; acceleration structure builds (`BuildAccelerationStructure` records the build or refit staged on the structure) | records the barriers before it, inside the pass's debug label |
| External | `AddExternalPass` | anything, through the raw `RHICommandContext` | records the barriers before it, inside the pass's debug label |

* `context.GetImage(texture)` returns the image behind a texture the pass declared. A Copy pass reaches buffers and acceleration structures only through its copy and build commands.
* Copies record no barrier of their own: the graph orders them through the declared accesses, and makes the data of a buffer the host reads visible to it.
* Every pass must leave each declared image in its declared layout, with no pending access beyond the declared one; the graph checks this after the pass records.
* Foreign code in an External pass that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## What the Compiler Decides

`Compile()` validates the declarations and plans the frame without recording anything. The [dump](#dump-format) shows every decision; culling and load/store carry their reasons in the dump's `cull_reason`, `load_reason` and `store_reason` fields.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import (every buffer and acceleration structure is one), or writes contents a later live pass uses. A write uses the previous contents unless it clears, or its pass fully overwrites and it does not also read. A culled pass stays in the dump, with a `cull_reason` naming its unread outputs.
* **Aliasing.** Transients whose lifetimes (first to last live pass) do not overlap share one image when format and pixel size match, assigned in order of first use, so a graph of the same shape maps each transient to the same image every frame. The dump's `physical` names each transient's image, and `first_use` and `last_use` its lifetime.
* **Pooling.** Images come from an `RGTexturePool`, which the owner of the graphs keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState` per subresource, for imports and transients alike, and the dump lists each pass's barriers with their layouts and accesses:
  * Each access moves its subresources into its layout and access with one barrier when they share a state, otherwise with one barrier per run of mips in one state within a layer.
  * A pooled image reused from the previous frame waits for that frame's last access.
  * A swap chain image's tracked access is `Present` from its creation and after each frame, so its first barrier waits for its acquire: on Vulkan a barrier from `Present` starts at the stage where the frame's submit waits for the acquired image.
  * A write discards the contents (`Undefined` source layout) when it clears, fully overwrites without reading, or writes a transient no earlier pass wrote.
  * Depth tests synchronize as depth writes, because the attachment's store op writes the depth image.
  * Buffers and acceleration structures synchronize through memory barriers by the same rule without layouts, starting from their tracked access (`RHITrackedAccess`).
  * A build also waits for earlier builds, which covers the BLAS it reads (built before the frame) and the scratch memory it reuses.
  * After the last pass, a buffer the host reads moves to `HostRead` through a barrier from its last access (the dump's `final_barrier`), unless it has none: waiting for the device does not make device writes visible to the host.
  * Barriers are batched per pass. On Metal the batches record nothing, but the plan and the tracked states are the same.
* **Load/store.** Per raster attachment, the load op is `Clear` if the access clears, `DontCare` if the pass fully overwrites it or it is a transient with no earlier writer, otherwise `Load`. The store op is `Store` if the next live pass touching the subresource uses its contents or the texture is imported, otherwise `DontCare`.

`Execute()` records each live pass: it writes the planned states through to the trackers of the images, buffers and acceleration structures, so foreign code and the next frame start from them, then records the pass's barrier batch and the pass. After the passes it records the barriers to `HostRead` in one batch.

## Errors

Declaration and recording errors log their message and abort in every build, including Release, where `ASSERT` compiles out. Under test, an `RGErrorsThrow` ([RGError.h](../libraries/include/renderer/graph/RGError.h)) makes the errors on its thread throw `RGError` with the message instead. The checks:

* an invalid handle, or subresources the texture does not have;
* an access the pass kind may not declare, or a texture subresource, buffer or acceleration structure declared twice by one pass;
* two attachments in one slot, a color slot out of range, or an attachment covering more than one subresource;
* `NativeAccess()` on a pass that is not a Raster pass;
* a transient without a format or size, or an import that is missing or multisampled;
* a Raster pass without attachments, or a Compute pass without an `RHIComputePass`;
* a transient read before any pass writes it;
* an import lacking the usages its accesses need;
* attachments of one pass that differ in size;
* a sampled binding of less than every subresource, or a storage binding of more than one mip;
* compiling twice, or executing before compiling or twice;
* a pass that reaches a texture, buffer or acceleration structure it did not declare through its context, or records raw commands without `NativeAccess()`;
* a pass that leaves a declared image in another layout, or with accesses beyond its declaration pending;
* a binding that no pipeline its pass drew or dispatched has, in a pass that drew or dispatched;
* a graph resource bound in a pipeline a Raster or Compute pass drew or dispatched, which the pass did not declare with the access of that binding: a `Texture2D` needs `Sampled` and a `StorageImage2D` a storage access, each covering the subresources of the bound view; an `AccelerationStructure` needs `AccelerationStructureRead`; a graph buffer may not be bound at all. The check reads the resource tables when the pass ends, skipping bindless arrays.

## Debugging

### Knobs

| cvar | default | Effect |
| --- | --- | --- |
| `render_graph_view` | *(empty)* | Shows a graph texture in place of the frame (see [Viewing a Texture](#viewing-a-texture)); also set from the render graph page. |
| `render_graph_cull` | `true` | Set to `false` to keep every pass, including those whose outputs no live pass uses. |
| `render_graph_full_barriers` | `false` | Makes each pass also wait for every access of the earlier passes of its graph, through one memory barrier on top of the planned ones. A synchronization bug that disappears with it lies in a planned barrier. It changes neither the plan nor the dump. |
| `validate_sync` | `false` | Vulkan synchronization validation, which reports hazards that no planned barrier orders; needs `validation` (see [Test.md](Test.md#validation-layer)). |

### Viewing a Texture

* `render_graph_view` names a graph texture to show in place of the frame, on every renderer.
* `AddPostChain` looks it up (`RenderGraph::FindTexture`) among the textures the frame's passes created or imported before the post chain. The `GraphView` pass then samples its final contents into Screen instead of the screen pass, so culling removes every pass that only fed the scene.
* The texture must be one a pass added then can sample through a 2D binding (`RenderGraph::CanSample2D`: a single-layer import with texture usage, or a transient an earlier pass writes), of a format a float texture samples (`SamplesAsFloat` in [Renderer.cpp](../libraries/source/renderer/renderer/Renderer.cpp)). Otherwise the frame shows as usual, with a warning logged once per change of the value.
* `GraphView` shows the values it samples (an sRGB texture decoded) as colors in Screen's format, which clips them, stretched over the output.
* For example, `IblBrdf` shows the BRDF map of a ready IBL, and `SceneDepth` on Deferred shows the depth `GBuffer` writes, with only `GBuffer`, `GraphView` and `Present` left live.

### The Render Graph Page

* The control panel's render graph page (the diagram icon) shows the live graph of the renderer's frame.
* While the page is drawn, the main thread asks the render thread each frame for the dump of its next graph, which `Renderer::ExecuteGraph` hands to `RenderFramework`, where the page reads the latest one under a mutex. Nothing is dumped while the page is closed.
* The page reads only the dump: passes in execution order with their kind and `gpu_ms` or, greyed, their cull reason; resources with their kind (a transient's format and size class) and their first and last use.
* Selecting a texture sets `render_graph_view` to its name, and `Frame` clears it. The page states which texture the frame shows (the one the `GraphView` pass samples), so selecting a texture that cannot be viewed shows the frame.
* `Save Graph Dump` writes the next graph to `screenshots/<scene>_<pipeline>_<time>.json` under the [external storage path](Run.md#external-storage-paths).

### Getting a Dump

* `RenderFramework::RequestGraphDump(name)` writes the next graph the renderer executes to `screenshots/<name>.json`; the page's `Save Graph Dump` and the test cases use it.
* On the render thread, `Renderer::RequestGraphDump(name, on_complete)` does the same, and `Renderer::RequestGraphDump(on_dump)` hands the next graph's dump to a callback instead.
* From the command line, the `render_graph_dump` test case writes the graph of the first frame that is ready for a screenshot to `screenshots/render_graph.json` under the [external storage path](Run.md#external-storage-paths) and exits; any other cvars select the frame:

```bash
python3 run.py --framework glfw --test_case render_graph_dump --headless true --pipeline deferred
python3 dev/render_graph_viewer.py <external-storage-path>/screenshots/render_graph.json
```

### Dump Format

`Dump()` returns the compiled graph as JSON:

* `passes`, in declaration order: each pass's `kind`, `culled` and `cull_reason`, `gpu_ms` once executed with a timer that has a result, `accesses`, `barriers` (images with layouts, buffers and acceleration structures without) and `attachments` (slot, load and store with their reasons). Each entry names its subresources unless it covers every one.
* `resources`, textures first, then buffers and acceleration structures: each resource's `type` (`Texture`, `Buffer` or `AccelerationStructure`) and `kind` (`Transient` or `Imported`); a transient's `format`, `size_class` (with the pixel size of an `Absolute` one) and `physical` image; the indices of the first and last live passes that use it with its `usage`, which for a buffer or acceleration structure is the union of its accesses; and the `final_barrier` of a buffer the host reads.
* The dump names size classes instead of pixel sizes, so a steady frame dumps the same passes, accesses, barriers and attachments at any resolution and on either backend. The `physical` assignment is the exception: transients of one format but different size classes share an image when their sizes resolve equal, e.g. `Scene` and `Output` at `render_scale` 1.

### Viewer

* `python3 dev/render_graph_viewer.py <dump.json> [-o <page.html>]` renders a dump as one self-contained static HTML page (next to the dump by default), using only the Python standard library.
* The page is a pass × resource grid: one row per pass in execution order, culled passes greyed with their reason, and one column per resource in dump order.
* A cell shows the pass's access (`R`, `W`, `RW`; `C` or `D` for a color or depth attachment with its load/store) and marks a barrier before the pass; hovering it details accesses, subresources, attachment reasons and barriers with layouts. Shaded cells span each resource's lifetime.
* A header counts passes, barriers and transient resources, and a `GPU ms` column appears when the dump has timings.

### Pass Timing

* `Execute(command_context, timers)` times Raster passes through an `RGPassTimers`, which holds one timed `RHIPass` per pass name; without one they are untimed. `Renderer::ExecuteGraph` passes the renderer's timers.
* A time arrives `max_frames_in_flight` frames after its pass records (see [RHIPass.h](../libraries/include/rhi/RHIPass.h)) while a graph lives one frame, so the owner of the graphs keeps the timers across frames. A pass's `gpu_ms` is the time its timer measured when the pass last ran in the current frame slot, at least `max_frames_in_flight` frames before the dumped frame.
* Passes sharing a name share a timer and report its last run.
* Compute passes report the time of their `RHIComputePass`, which measures only when created timed.
* Copy and External passes are untimed: a Metal timer covers one encoder, while a Copy pass opens one per copy, and foreign code times its own work (NRD, MetalFX).
* Timing needs `RHIContext::SupportsPassTimestamps()`.

## Renderers

Each renderer builds one graph per frame from the `RenderFramework`'s texture pool, which outlives renderer recreation, so a pipeline switch reuses the images both pipelines' graphs need. The goldens in [tests/render_graph/golden/](../tests/render_graph/golden/) list the passes, accesses, barriers, attachments and resources of a typical frame of each renderer (see [Tests](#tests)); the [viewer](#viewer) renders a dump of any frame.

Every frame ends with `Renderer::AddPostChain(graph, scene, screen_pass)`:

* The screen pass (`ToneMapping` or `Upsample`), when given, draws `scene` into Screen, a transient at output resolution whose format the renderer chooses once (`Renderer::InitPostChain`): `B8G8R8A8Srgb` for the renderers that tone map on the GPU, the CPU renderer's `RGBAFloat16` otherwise. Without a screen pass, `scene` is the screen; `GraphView` replaces the screen pass while `render_graph_view` shows a texture.
* `Readback` is a Copy pass that copies the screen into a staging buffer the host reads, when a screenshot is pending: before `Ui` for a screenshot without UI, after it for one with UI, which gets the screen without UI when `Ui` does not draw. The buffer is created with the pass (its size comes from `RenderGraph::GetFormat` and `GetSize`), imported, and saved once the frame completes.
* `Ui` (UI shown, not headless) draws ImGui into the rendering the graph begins over Screen, through `NativeAccess()`. The ImGui backend compiles its pipelines for Screen's format (`RHIUiHandler::Setup` takes an attachment signature).
* `Present` draws the screen into BackBuffer, the image `RHIContext::GetBackBuffer()` returns: a windowed Vulkan device's acquired swap chain image, otherwise one image whose Metal texture is the frame's drawable when windowed.
* The screen passes and `Present` are `ScreenQuadPass`es built from their output format and the filter they sample their input with, always edge-clamped (`ScreenQuadPass::InputFilter`). `ToneMapping`, `Upsample` and `GraphView` sample bilinearly when the input's size differs from the output's and the device filters the input's format linearly (`RHIContext::SupportsLinearFiltering`: linear filtering of 32-bit float and depth formats is optional on both backends), otherwise nearest. `Present` samples nearest and applies the window's pre-rotation.

The renderers:

* **CPU.** The path tracer tone maps on the host into a host buffer, imported as HostSceneColor. The `Upload` Copy pass copies it into the SceneColor transient at scene resolution, which `Upsample` draws into the screen when `render_scale` < 1 and which is the screen otherwise.
* **GPU.** The accumulator, the TLAS, the denoiser inputs (once a denoiser has needed them) and the denoiser outputs are imports. `ClearAccumulator` (a Raster pass with no draws) clears the accumulator when the camera moved or the scene changed, `BuildTLAS` (Copy) records the build or refit `GPURenderer::Update` staged on the TLAS, and `PathTrace` (Compute, on the renderer's timed `RHIComputePass`, which dynamic spp reads back) traces while accumulating. `PathTrace` declares the six denoiser inputs as storage writes once they are allocated, because the tracer binds them on every dispatch. A denoiser adds one External pass (see [Denoiser.md](Denoiser.md)). Tone mapping displays the provider's output when it encodes the frame, the accumulator when a frame traces without denoising or the provider cannot encode it, and otherwise whatever it displayed last (a converged or paused frame keeps the last denoised output, imported as DenoiserOutput).
* **Forward.** `DirectionalShadow` renders the ShadowMap transient (`shadow_map_resolution` squared) when the scene has a directional light, `BasePass` writes the SceneColor and SceneDepth transients, and `SkyBox` draws the sky map over them while depth-testing. The IBL maps and the sky map are imports, and an IBL map is imported only once ready. `BasePass` samples the shadow map and IBL maps as `LightingInputs`, where a missing one (no directional light, a map still cooking) samples a placeholder; `SkyBox` samples the map the output mode selects, with the sampler it is read with (`Renderer::GetSkyBoxMap`). Tone mapping upsamples when `render_scale` < 1, so the graph has the same passes at any scale.
* **Deferred.** The same shadow, IBL maps, sky box and post chain as Forward, with `GBuffer` writing the GBufferPacked and SceneDepth transients and `Lighting` sampling them into SceneColor. Lighting reconstructs world positions from the scene depth it samples and the sky box then depth-tests against it, so the graph moves SceneDepth from the depth attachment layout to `Read` and back within the frame.
* **IBL cook.** While an IBL map cooks on the GPU, `ImageBasedLighting::AddCookPasses` starts the Forward or Deferred graph with one cook step per map still cooking. The first step clears each subresource of the map being cooked in its own Raster pass (`ClearIblBrdfCook`, `ClearIblDiffuseCook`, `ClearIblSpecularCook`). `CookIblBrdf`, `CookIblDiffuse` (all six faces as a 2D array) and `CookIblSpecular` (the mip being cooked) are Compute passes that read and write the cooking map as storage; the diffuse and specular cooks also sample the sky map. The cooking maps are other images than the maps the scene passes sample, and a finished map leaves its cook with every subresource in one tracked state. `IblCookAccelerator` drives a pass to completion in frames of its own, each a graph holding one cook step.

## Tests

* `render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans (culling, image sharing within and across frames, per-subresource barriers and mip runs, load/store with reasons, buffer and acceleration-structure barriers, full barriers, bindings and placeholders), executes them and reads the results back ([RenderGraphCompileTest.cpp](../tests/render_graph/RenderGraphCompileTest.cpp)). `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)).
* `render_graph_errors` makes one mistake per error with errors thrown, and expects each error's message.
* `render_graph_pass_timing` executes a graph of a Compute and a Raster pass every frame with pass timers kept across frames, and expects both passes to dump a `gpu_ms` once their frame slot returns, or none on a device without pass timestamps.
* `pipeline_switch_pool` switches pipelines at runtime and requires each new renderer to reuse an image the previous one left in the texture pool.
* [tests/build_system/test_render_graph_viewer.py](../tests/build_system/test_render_graph_viewer.py) unit-tests the viewer and the golden projection.

### Golden Graph Shapes

* Each renderer has a golden graph shape in [tests/render_graph/golden/](../tests/render_graph/golden/) (`<pipeline>.txt`), checked by a `<pipeline>_graph_shape` registry case. `deferred_graph_view_shape` checks a deferred frame viewing SceneDepth against `deferred_view.txt`, and `deferred_graph_view_fallback` one naming the integer GBufferPacked against `deferred.txt`.
* The shape cases run the `render_graph_dump` test case, which waits until the scene is ready for a screenshot and asks the renderer (`RenderFramework::RequestGraphDump`) to write the next graph it executes to `screenshots/render_graph.json`, which the device runners pull like screenshots.
* A converged GPU frame traces nothing, so `gpu_graph_shape` runs `render_graph_dump_accumulating`, which dumps frames once the scene is loaded until one traces onto samples the accumulator already holds with no TLAS build (a live `PathTrace`, no `ClearAccumulator` or `BuildTLAS`), so the frame does not depend on when loading finished. Its golden is a frame without a denoiser, clear or screenshot, and needs ray query support (lavapipe has it).
* [tests/render_graph/graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, attachment and resource, leaving out GPU times, and prints a unified diff against the golden.
* It also renders the dump through the viewer to `screenshots/captures/render_graph_<case>.html` (its `--page`) and fails the case if that raises. CI uploads the pages with the test screenshots (the `test-screenshots-<framework>-<os>` artifact).
* The goldens are frames of the default TestScene, so the shape cases ignore the suite's `--scene`. The forward and deferred goldens are frames with a directional light, a sky map and ready IBL maps.
* Tests run headless, so the goldens show no `Ui` pass, and the back buffer's first barrier has no `Present` access to wait for.

### Updating a Golden

A change that alters a graph on purpose updates its golden in two steps: run the shape case, whose compare step fails with the diff and leaves the dump in the screenshots folder, then rewrite the golden from that dump with the case's `--golden` name. `dev/run_tests.py` passes unknown arguments to the app, not to the evaluator, so `--update` goes to `graph_shape_test.py` directly. Each shape case overwrites the same dump, so update one golden per run.

A golden is a shape gate, not a pure function of the renderer's code. Besides a change to the passes or their declarations, these change it:

* the previous frame's graph: a transient's or import's first barrier starts from the access the previous frame left in the image's tracked state;
* the pool's order: a transient gets the first free matching image in the order the pool created them, so earlier graphs (another pipeline's, an IBL cook frame's) decide which image it gets and which earlier access its first barrier waits for;
* the scene: a directional light, a sky map and ready IBL maps add passes and imports;
* `shadow_map_resolution`: the ShadowMap's pixel size is in the golden;
* the resolution: transients whose sizes resolve equal may share an image, which changes `physical`.

```bash
python3 dev/run_tests.py --framework glfw --config Release --case deferred_graph_shape
python3 tests/render_graph/graph_shape_test.py --framework glfw --golden deferred --update
```
