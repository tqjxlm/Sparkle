# Render Graph

The render graph ([libraries/include/renderer/graph/RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h)) records one frame's GPU passes from declared accesses. Passes state what they read and write in textures, buffers and acceleration structures; the graph derives culling, transient images, image layouts, barriers and attachment load/store actions, and can dump every decision it made.

Every renderer records its frame through the graph. The RHI keeps only the lowering primitives: `RHICommandContext::BeginRendering` over an `RHIRenderingInfo` (attachments, load/store and clear values, with the attachments already in their attachment layouts), access-based barriers, and PSOs compiled per attachment signature. RHI tests ([tests/rhi/](../tests/rhi/)) record through these primitives directly.

## Building a Graph

A graph lives for one frame: build it, `Compile()`, `Execute(command_context)`, then destroy it.

```cpp
RenderGraph graph(rhi, texture_pool, render_config);
auto scene_color = graph.CreateTexture("SceneColor", {.format = PixelFormat::RGBAFloat16, .size_class = RGSizeClass::Scene});
auto history = graph.Import("History", history_image);

graph.AddRasterPass("Lighting", [&](RGBuilder &b) {
    b.Sampled(history, &LightingPixelShader::ResourceTable::history);
    b.ColorWrite(scene_color, 0, Vector4(0, 0, 0, 1));
    return [this](RGRasterContext &ctx) { ctx.DrawMesh(pso_, draw_args_); };
});

graph.Compile();
graph.Execute(*rhi->GetCommandContext());
```

* **Textures.** `CreateTexture` makes a transient: single-sampled, one mip, with a size class resolved from `RenderResolution` (`Scene`, `Output`, or `Absolute` with an explicit size). Its image usage is the union of its declared accesses. `Import` brings in a persistent image (history, swap chain, IBL maps); importing an image again returns the texture, and name, of its first import, so passes that sample the same image need not coordinate.
* **Subresources.** A texture access covers every mip and array layer, or the ones `texture.Mip(mip)` (a mip of every layer) or `texture.Subresource(mip, layer)` select, e.g. the face of a cube map a clear writes. An attachment is one subresource; a pass may declare disjoint subresources of one texture.
* **Buffers and acceleration structures.** `Import` also brings in a persistent buffer (`RGBuffer`: staging and host buffers) or top-level acceleration structure (`RGAccelerationStructure`); importing one again returns its first import. Every buffer and acceleration structure is an import.
* **Handles.** `RGTexture`, `RGBuffer` and `RGAccelerationStructure` are plain indices. Passes run in declaration order, and every access depends on the last write before it in that order, so handles need no versions.
* **Accesses.** Textures: `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`. Buffers: `CopySrc`, `CopyDst`. Acceleration structures: `AccelerationStructureBuild` (a build or refit) and `AccelerationStructureRead` (ray queries). Shader accesses take an optional stage mask; the default is the pass kind's stage (raster: pixel, compute: compute, external: all). A pass declares each texture, buffer and acceleration structure once. `FullyOverwrites()` states that the pass writes every texel of the textures it writes, which discards the previous contents of those it does not also read; `SideEffect()` keeps a pass whose outputs nothing reads; `NativeAccess()` lets a Raster pass record foreign commands (ImGui) through the raw command context, inside the rendering the graph begins over its attachments.
* **Bindings.** A shader access (`Sampled`, `StorageWrite`, `StorageReadWrite`) may name the binding member of a shader's `ResourceTable` that reads the texture, e.g. `&ToneMappingPixelShader::ResourceTable::screenTexture` (a `Texture2D` member for `Sampled`, a `StorageImage2D` member for storage accesses). While the pass records, every pipeline drawn or dispatched through the command context binds a view of the texture there (a sampled access's default view of the whole image; a storage access's view of its single mip, with a cube's layers as a 2D array), in each of its resource tables of that type, so each input is written once and serves every pipeline of the pass that uses the table. A `Sampled` access may also name a `Sampler` member, e.g. `&ToneMappingPixelShader::ResourceTable::screenTextureSampler`, together with the attributes of the sampler the pass samples the texture with, which binds that sampler there (from `RHIContext::GetSampler`'s cache): images carry no sampler, so the pass that samples a texture decides how. Samplers shared by several passes live on the class that owns the sampled resource: `SkyRenderProxy::SkyMapSampler` for the sky map (the sky box and the IBL cooks) and `ImageBasedLighting::MapSampler` for the IBL maps. An input that may be missing is declared with `SampledOrPlaceholder`: an invalid texture binds the given placeholder image, which is outside the graph, with the given sampler instead, so no pipeline keeps an image an earlier graph bound. `AccelerationStructureRead` may name an `AccelerationStructure` member, which binds the acceleration structure there. Every binding must reach at least one pipeline the pass draws or dispatches, unless the pass draws nothing (an empty scene). Views are created at compile. Uniform buffers and other resources outside the graph stay bound by the pass. A pipeline keeps what earlier passes and frames bound, so a Raster or Compute pass must declare every graph resource bound in the pipelines it draws or dispatches, whoever bound it.
* **Pass kinds.** Each kind's execute function receives a context that exposes only what the kind may record:

| Kind | Context records | The graph around it |
| --- | --- | --- |
| Raster | draws; with `NativeAccess()`, anything the open rendering allows through `GetNativeContext()`, and the foreign code resets the backend state it records around | begins and ends rendering over the declared attachments; the opening barriers sit inside the pass's debug label and timer |
| Compute | dispatches | brackets it with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | copies between images and buffers; acceleration structure builds (`BuildAccelerationStructure` records the build or refit staged on the structure) | records the barriers before it, inside the pass's debug label |
| External | anything, through the raw `RHICommandContext` | records the barriers before it, inside the pass's debug label |

`ctx.GetImage(texture)` returns the image behind a texture the pass declared; a Copy pass reaches buffers and acceleration structures only through its copy and build commands. Every pass must leave each declared image in its declared layout, with no pending access beyond the declared one, which the graph checks after the pass records; foreign code in an External pass that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## Compilation

`Compile()` records nothing. Declaration errors abort in every build, including Release: an invalid or doubly declared handle, an access the pass kind may not declare, two attachments in one slot, a raster pass without attachments, a transient read before any pass writes it, attachments of different sizes, an import lacking the usages its accesses need, a pass that leaves a declared image in another state, a binding that no pipeline its pass drew or dispatched has, in a pass that drew or dispatched, and a graph resource bound in a pipeline a Raster or Compute pass drew or dispatched that the pass did not declare with the access of that binding: a `Texture2D` needs `Sampled` and a `StorageImage2D` a storage access, each covering the subresources of the bound view; an `AccelerationStructure` needs `AccelerationStructureRead`; a graph buffer may not be bound at all. The check reads the resource tables when the pass ends, skipping bindless arrays. Under test, an `RGErrorsThrow` ([RGError.h](../libraries/include/renderer/graph/RGError.h)) makes the errors on its thread throw `RGError` with the message instead of aborting.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import (every buffer and acceleration structure is one), or writes contents a later live pass uses. A write uses the previous contents unless it clears, or its pass fully overwrites and it does not also read. Culled passes are dumped with their unread outputs. `render_graph_cull` (default `true`) turns culling off.
* **Transients.** Transients whose lifetimes (first to last live pass) do not overlap share one image when format, extent and sampler match, in order of first use, so a graph of the same shape maps each transient to the same image every frame. Images come from an `RGTexturePool`, which the owner of the graph keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState` per subresource, for imports and transients alike, and applies `TransitionImageState` per access: one barrier when the access's subresources share a state, otherwise one per run of mips in one state within a layer. A pooled image reused from the previous frame therefore waits for that frame's last access, and a swap chain image, whose tracked access is `Present` from its creation and after each frame, waits for its acquire: on Vulkan a barrier from `Present` starts at the stage where the frame's submit waits for the acquired image. A write discards the contents (`Undefined` source layout) when it clears, fully overwrites without reading, or writes a transient no earlier pass wrote. Barriers are batched per pass. Depth tests synchronize as depth writes, because the attachment store op writes the depth image. Buffers and acceleration structures synchronize through memory barriers by the same rule without layouts, starting from their tracked access (`RHITrackedAccess`); a build also waits for earlier builds, which covers the BLAS it reads (built before the frame) and the scratch memory it reuses. On Metal the batches record nothing, but the plan and the tracked states are the same. `render_graph_full_barriers` (default `false`) makes each pass also wait for every access of the earlier passes of its graph, through one memory barrier on top of the planned ones, so a synchronization bug that disappears with it lies in a planned barrier; it changes neither the plan nor the dump.
* **Load/store.** Per raster attachment: `Clear` if declared, `DontCare` if fully overwritten or a transient with no earlier writer, otherwise `Load`. `Store` if the next live pass touching the subresource uses its contents or the texture is imported, otherwise `DontCare`. Each choice carries its reason.

`Execute()` records each live pass: it writes the planned states through to the images', buffers' and acceleration structures' trackers, so foreign code and the next frame start from them, then records the barrier batch and the pass.

`Execute(command_context, timers)` times Raster passes through an `RGPassTimers`, which holds one timed `RHIPass` per pass name; without one they are untimed. A time arrives `max_frames_in_flight` frames after its pass records (see [RHIPass.h](../libraries/include/rhi/RHIPass.h)) while a graph lives one frame, so the owner of the graphs keeps the timers across frames; passes sharing a name share a timer and report its last run. Compute passes report the time of their `RHIComputePass`, which measures only when created timed. Copy and External passes are untimed: a Metal timer covers one encoder, while a Copy pass opens one per copy, and foreign code times its own work (NRD, MetalFX). Timing needs `RHIContext::SupportsPassTimestamps()`.

## Renderers

A renderer builds one graph per frame from the `RenderFramework`'s texture pool, which outlives renderer recreation so a pipeline switch reuses the images both pipelines' graphs need (`pipeline_switch_pool` checks it), and hands it to `Renderer::ExecuteGraph`, which compiles it, records it with the renderer's pass timers, and then dumps it when a dump is requested: to a file (`Renderer::RequestGraphDump(name, on_complete)`) or to a callback (`Renderer::RequestGraphDump(on_dump)`, for the next graph only). Every renderer ends its frame with `Renderer::AddPostChain(graph, scene, screen_pass)`, whose passes are Raster passes unless stated:

| Pass | Declares |
| --- | --- |
| the screen pass (`ToneMapping` or `Upsample`), when given, or `GraphView` showing a graph texture | `Sampled` scene or the viewed texture, `ColorWrite` Screen (fully overwritten) |
| `Readback` (screenshot without UI) | Copy pass: `CopySrc` Screen, `CopyDst` ScreenshotBuffer, a staging buffer created with the pass (its size comes from `RenderGraph::GetFormat` and `GetSize`) and saved once the frame completes |
| `Ui` (UI shown, not headless) | `ColorWrite` Screen, `NativeAccess()` for ImGui |
| `Readback` (screenshot with UI) | as above; without `Ui` it reads Screen without the UI |
| `Present` | `Sampled` Screen, `ColorWrite` BackBuffer (fully overwritten), the image `RHIContext::GetBackBuffer()` returns: a windowed Vulkan device's acquired swap chain image, otherwise one image whose Metal texture is the frame's drawable when windowed |

Screen is a transient at output resolution whose format the renderer chooses once (`Renderer::InitPostChain`): `B8G8R8A8Srgb` for the renderers that tone map on the GPU, the CPU renderer's `RGBAFloat16` otherwise. Without a screen pass, `scene` is the screen. The screen passes and `Present` are `ScreenQuadPass`es built from their output format and the filter they sample their input with, always edge-clamped (`ScreenQuadPass::InputFilter`). `ToneMapping`, `Upsample` and `GraphView` sample bilinearly when the input's size differs from the output's and the device filters the input's format linearly (`RHIContext::SupportsLinearFiltering`: linear filtering of 32-bit float and depth formats is optional on both backends), otherwise nearest. `Present` samples nearest and applies the window's pre-rotation. `Ui` draws into the rendering the graph begins over Screen, and the ImGui backend compiles its pipelines for Screen's format (`RHIUiHandler::Setup` takes an attachment signature).

The `render_graph_view` config (empty by default) names a graph texture to show in place of the frame, on every renderer: `AddPostChain` looks it up (`RenderGraph::FindTexture`) among the textures the frame's passes created or imported before the post chain, and `GraphView` samples its final contents into Screen instead of the screen pass, so culling removes every pass that only fed the scene. The texture must be one a pass added then can sample through a 2D binding (`RenderGraph::CanSample2D`: a single-layer import with texture usage, or a transient an earlier pass writes) and of a format a float texture samples (not an integer or depth-stencil format); otherwise the frame shows as usual, with a warning logged once per change of the value. `GraphView` shows the values it samples (an sRGB texture decoded) as colors in Screen's format, which clips them, stretched over the output. For example, `IblBrdf` shows the BRDF map of a ready IBL, and `SceneDepth` on Deferred the depth `GBuffer` writes, with only `GBuffer`, `GraphView` and `Present` left live.

The CPU renderer runs on the graph. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `Upload` | Copy | `CopySrc` HostSceneColor, the host buffer the path tracer filled; `CopyDst` SceneColor (fully overwritten) |
| `Upsample` (`render_scale` < 1), `Readback`, `Ui`, `Present` | | the post chain; without upsampling SceneColor is the screen |

SceneColor is a transient at scene resolution, already tone-mapped on the CPU.

The GPU renderer runs on the graph. The accumulator, the TLAS, the path-tracing denoiser inputs (once a denoiser has needed them) and the denoiser's output (and NRD's output history) are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `ClearAccumulator` (camera moved or scene changed) | Raster | `ColorWrite` Accumulator, cleared to zero; no draws |
| `BuildTLAS` (primitives changed) | Copy | `AccelerationStructureBuild` TLAS |
| `PathTrace` (accumulating) | Compute | `AccelerationStructureRead` TLAS; `StorageReadWrite` Accumulator; `StorageWrite` of the six denoiser inputs once allocated, because the tracer binds them as storage images on every dispatch (dummies until then); each declaration binds its image to the tracer |
| `Nrd` (NRD encodes) | External | `Sampled` (compute) of the six inputs and Accumulator; `StorageWrite` NrdOutput, `StorageReadWrite` NrdOutputHistory |
| `MetalFx` (MetalFX encodes) | External | `Sampled` (compute) of the four auxiliary inputs and Accumulator; `StorageWrite` of the displayed image: MetalFxOutput (the scaler's output) before the handoff starts, MetalFxResolvedOutput after |
| `ToneMapping`, `Readback`, `Ui`, `Present` | | the post chain from the displayed texture |

`GPURenderer::Update` stages the primitive changes on the TLAS: `RHITLAS::Build` and `Update` build dirty BLAS in their own submits (Metal compacts them, committing the frame's command buffer and waiting), write the instances and allocate the structure, and `BuildTLAS` records the staged build or refit. `PathTrace` runs on the renderer's timed `RHIComputePass`, which dynamic spp reads back. The displayed texture is the provider's output when it encodes this frame, the Accumulator when a frame traces without denoising or the provider cannot encode it, and otherwise whatever tone mapping displayed last (a converged or paused frame keeps the last denoised output, imported as DenoiserOutput). A denoiser's pass keeps its internal textures private: NRD's pool and `IN_*`/`OUT_*` textures and MetalFX's prepared textures (and its scaler output while it resolves) keep their own transitions, and every image the pass declares ends in its declared state.

The Forward renderer runs on the graph. The directional shadow map, scene color and scene depth are transients; the IBL maps and the sky map are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `DirectionalShadow` (directional light) | Raster | `DepthWrite` ShadowMap (`shadow_map_resolution` squared), cleared |
| `BasePass` | Raster | `Sampled` ShadowMap and each ready IBL map (IblBrdf, IblDiffuse, IblSpecular); `ColorWrite` SceneColor and `DepthWrite` SceneDepth, both cleared |
| `SkyBox` (sky map) | Raster | `Sampled` of the sky map it draws (an IBL map in the IBL map output modes); `ColorWrite` SceneColor, `DepthTest` SceneDepth |
| `ToneMapping`, `Readback`, `Ui`, `Present` | | the post chain from SceneColor |

Tone mapping upsamples when `render_scale` < 1, so the graph has the same passes at any scale. The renderer passes each pass its inputs every frame: the shadow map and IBL maps as `LightingInputs`, where a missing one (no directional light, a map still cooking) samples a placeholder, and the sky box's map, with the sampler it is read with, as the output mode selects it (`Renderer::GetSkyBoxMap`). IBL maps are imported only once ready. While a map cooks on the GPU, `ImageBasedLighting::AddCookPasses` starts the frame's graph with one cook step per map still cooking:

| Pass | Kind | Declares |
| --- | --- | --- |
| `ClearIblBrdfCook`, `ClearIblDiffuseCook`, `ClearIblSpecularCook` (first step, once per subresource) | Raster | `ColorWrite` of one mip of one layer of the map being cooked, cleared; no draws |
| `CookIblBrdf` | Compute | `StorageReadWrite` IblBrdfCook, bound to the cook shader |
| `CookIblDiffuse` | Compute | `Sampled` SkyMap, the environment map it integrates; `StorageReadWrite` IblDiffuseCook (all six faces as a 2D array); both bound to the cook shader |
| `CookIblSpecular` | Compute | `Sampled` SkyMap; `StorageReadWrite` of the mip of IblSpecularCook being cooked; both bound to the cook shader |

The cooking maps are other images than the maps the scene passes sample. A finished map leaves its cook with every subresource in one tracked state, and its readback (`IBLPass::Finalize`) records in its own command buffer once the frame completes. `IblCookAccelerator` drives a pass to completion in frames of its own, each a graph holding one cook step.

The Deferred renderer runs on the graph with the same shadow, IBL maps, sky box and post chain as Forward; the packed GBuffer is a transient too. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `DirectionalShadow` (directional light) | Raster | as in Forward |
| `GBuffer` | Raster | `ColorWrite` GBufferPacked and `DepthWrite` SceneDepth, both cleared |
| `Lighting` | Raster | `Sampled` of GBufferPacked, SceneDepth, ShadowMap and each ready IBL map; `ColorWrite` SceneColor (fully overwritten) |
| `SkyBox` (sky map) | Raster | as in Forward: `Sampled` sky map, `ColorWrite` SceneColor, `DepthTest` SceneDepth |
| `ToneMapping`, `Readback`, `Ui`, `Present` | | as in Forward |

Lighting reconstructs world positions from the scene depth it samples, and the sky box then depth-tests against it, so the graph moves SceneDepth from the depth attachment layout to `Read` and back within the frame.

## Dump

`Dump()` returns the compiled graph as JSON: passes (kind, culled with reason, `gpu_ms` once executed with a timer that has a result, accesses, barriers with layouts and accesses, attachments with load/store and reasons, each naming its subresources unless it covers every one) and resources (type: `Texture`, `Buffer` or `AccelerationStructure`; kind, format, size class, the indices of the first and last live passes that use it, usage, physical image). Buffers and acceleration structures follow the textures; their barriers have no layouts, and their usage is the union of their accesses. It names size classes instead of pixel sizes, so a graph dumps the same at any resolution and on either backend. A pass's `gpu_ms` is the time its timer measured when the pass last ran in the current frame slot, at least `max_frames_in_flight` frames before the dumped frame.

`python3 dev/render_graph_viewer.py <dump.json> [-o <page.html>]` renders a dump as one self-contained static HTML page (next to the dump by default), using only the Python standard library. The page is a pass × resource grid: one row per pass in execution order, culled passes greyed with their reason, and one column per resource in dump order. A cell shows the pass's access (`R`, `W`, `RW`; `C` or `D` for a color or depth attachment with its load/store), marks a barrier before the pass, and details accesses, subresources, attachment reasons and barriers with layouts on hover; shaded cells span each resource's lifetime. A header counts passes, barriers and transient resources, and a `GPU ms` column appears when the dump has timings.

The control panel's render graph page (the diagram icon) shows the live graph of the renderer's frame. While the page is drawn, the main thread asks the render thread each frame for the dump of its next graph, which `Renderer::ExecuteGraph` hands to `RenderFramework`, where the page reads the latest one under a mutex; nothing is dumped while the page is closed. The page reads only the dump: passes in execution order with their kind, `gpu_ms` or, greyed, their cull reason, and resources with their kind (a transient's format and size class), first and last use. Selecting a resource sets `render_graph_view` to its name and `Frame` clears it; the selected row is highlighted, and the page states which texture the frame shows, the one the `GraphView` pass samples, so a texture that cannot be viewed shows the frame. `Save Graph Dump` writes the next graph to `screenshots/<scene>_<pipeline>_<time>.json` through `RenderFramework::RequestGraphDump`.

## Tests

`render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans, executes them and reads results back, including a screen quad whose pipeline was created from an attachment signature with nothing bound and draws the texture and sampler its pass declared, a texture copied through a buffer whose memory barrier orders the copies (also with `render_graph_full_barriers`), accesses to subresources of a cube map with the barriers and store actions they plan per subresource, mips left in one state moving with one barrier over their run, a pass that fully overwrites one texture while it reads and writes another, which keeps the other's contents, a quad whose missing input draws its placeholder, a raster pass recording through its native context inside the graph's rendering, a buffer imported twice, and, with hardware ray tracing, acceleration structure builds that wait for earlier builds and a ray query that waits for them. `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)). `render_graph_errors` makes one mistake per error with errors thrown, and expects each error's message. `render_graph_pass_timing` executes a graph of a Compute and a Raster pass every frame with pass timers kept across frames and expects both passes to dump a `gpu_ms` once their frame slot returns, or none on a device without pass timestamps.

Each renderer on the graph has a golden graph shape in [tests/render_graph/golden/](../tests/render_graph/golden/) (`<pipeline>.txt`), checked by a `<pipeline>_graph_shape` registry case. The goldens are frames of the default TestScene, so the shape cases ignore the suite's `--scene`. The `render_graph_dump` test case waits until the scene is ready for a screenshot and asks the renderer (`RenderFramework::RequestGraphDump`) to write the next graph it executes to `screenshots/render_graph.json`, which the device runners pull like screenshots. [tests/render_graph/graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, attachment and resource, leaving out GPU times, and prints a unified diff against the golden; `--update` rewrites the golden from the dump. It also renders the dump through the viewer to `screenshots/captures/render_graph_<case>.html` (its `--page`), and fails the case if that raises; CI uploads the pages with the test screenshots (the `test-screenshots-<framework>-<os>` artifact). A converged GPU frame traces nothing, so `gpu_graph_shape` runs `render_graph_dump_accumulating`, which dumps frames once the scene is loaded until one traces onto samples the accumulator already holds with no TLAS build (a live `PathTrace`, no `ClearAccumulator` or `BuildTLAS`), so the frame does not depend on when loading finished; the golden is a frame without a denoiser, clear or screenshot, and needs ray query support (lavapipe has it). The forward and deferred goldens are frames with a directional light, a sky map and ready IBL maps. `deferred_graph_view_shape` checks a deferred frame viewing `SceneDepth` against `deferred_view.txt`, and `deferred_graph_view_fallback` one naming the integer `GBufferPacked` against the normal deferred golden. Tests run headless, so the goldens show no `Ui` pass, and the back buffer's first barrier has no present access to wait for.
