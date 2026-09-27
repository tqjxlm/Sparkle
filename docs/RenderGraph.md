# Render Graph

The render graph ([libraries/include/renderer/graph/RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h)) records one frame's GPU passes from declared accesses. Passes state what they read and write in textures, buffers and acceleration structures; the graph derives culling, transient images, image layouts, barriers and attachment load/store actions, and can dump every decision it made.

## Building a Graph

A graph lives for one frame: build it, `Compile()`, `Execute(command_context)`, then destroy it.

```cpp
RenderGraph graph(texture_pool, render_config);
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
* **Accesses.** Textures: `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `StorageRead`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`. Buffers: `CopySrc`, `CopyDst`. Acceleration structures: `AccelerationStructureBuild` (a build or refit) and `AccelerationStructureRead` (ray queries). Shader accesses take an optional stage mask; the default is the pass kind's stage (raster: pixel, compute: compute, external: all). A pass declares each texture, buffer and acceleration structure once. `FullyOverwrites()` states that the pass writes every texel, which discards previous contents; `SideEffect()` keeps a pass whose outputs nothing reads; `NativeAccess()` lets a Raster pass record foreign commands (ImGui) through the raw command context, inside the rendering the graph begins over its attachments.
* **Bindings.** A shader access (`Sampled`, `StorageRead`, `StorageWrite`, `StorageReadWrite`) may name the binding member of a shader's `ResourceTable` that reads the texture, e.g. `&ToneMappingPixelShader::ResourceTable::screenTexture` (a `Texture2D` member for `Sampled`, a `StorageImage2D` member for storage accesses). While the pass records, every pipeline drawn or dispatched through the command context binds a view of the texture there (a sampled access's default view of the whole image; a storage access's view of its single mip, with a cube's layers as a 2D array), in each of its resource tables of that type, so each input is written once and serves every pipeline of the pass that uses the table. A `Sampled` access may also name a `Sampler` member, e.g. `&ToneMappingPixelShader::ResourceTable::screenTextureSampler`, which binds the sampler the texture's image carries (`RGTextureDesc::sampler` for transients), so a pass holds no sampler state for its inputs. An input that may be missing is declared with `SampledOrPlaceholder`: an invalid texture binds the given placeholder image, which is outside the graph, and its sampler instead, so no pipeline keeps an image an earlier graph bound. `AccelerationStructureRead` may name an `AccelerationStructure` member, which binds the acceleration structure there. Every binding must reach at least one pipeline the pass draws or dispatches, unless the pass draws nothing (an empty scene). Views are created at compile. Uniform buffers and other resources outside the graph stay bound by the pass.
* **Pass kinds.** Each kind's execute function receives a context that exposes only what the kind may record:

| Kind | Context records | The graph around it |
| --- | --- | --- |
| Raster | draws; with `NativeAccess()`, anything the open rendering allows through `GetNativeContext()`, and the foreign code resets the backend state it records around | begins and ends rendering over the declared attachments; the opening barriers sit inside the pass's debug label |
| Compute | dispatches | brackets it with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | copies between images and buffers; acceleration structure builds (`BuildAccelerationStructure` records the build or refit staged on the structure) | records the barriers before it |
| External | anything, through the raw `RHICommandContext` | records the barriers before it, then checks the contract below |

`ctx.GetImage(texture)`, `ctx.GetBuffer(buffer)` and `ctx.GetAccelerationStructure(acceleration_structure)` return the resource behind a handle the pass declared. An External pass must leave every declared image in its declared layout, with no pending access beyond the declared one; foreign code that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## Compilation

`Compile()` records nothing. Declaration errors abort in every build, including Release: an invalid or doubly declared handle, an access the pass kind may not declare, two attachments in one slot, a raster pass without attachments, a transient read before any pass writes it, attachments of different sizes, an import lacking the usages its accesses need, a broken External contract, and a binding that no pipeline its pass drew or dispatched has, in a pass that drew or dispatched.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import (every buffer and acceleration structure is one), or writes contents a later live pass uses. A write uses the previous contents unless it clears or its pass fully overwrites. Culled passes are dumped with their unread outputs. `render_graph_cull` (default `true`) turns culling off.
* **Transients.** Transients whose lifetimes (first to last live pass) do not overlap share one image when format, extent and sampler match, in order of first use, so a graph of the same shape maps each transient to the same image every frame. Images come from an `RGTexturePool`, which the owner of the graph keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState` per subresource, for imports and transients alike, and applies `TransitionImageState` per access: one barrier when the access's subresources share a state, otherwise one per run of mips in one state within a layer. A pooled image reused from the previous frame therefore waits for that frame's last access, and a swap chain image's first attachment write chains to the acquire wait because attachment writes add their own access to the barrier source. A write discards the contents (`Undefined` source layout) when it clears, fully overwrites, or writes a transient no earlier pass wrote. Barriers are batched per pass. Depth tests synchronize as depth writes, because the attachment store op writes the depth image. Buffers and acceleration structures synchronize through memory barriers by the same rule without layouts, starting from their tracked access (`RHITrackedAccess`); a build also waits for earlier builds, which covers the BLAS it reads (built before the frame) and the scratch memory it reuses. On Metal the batches record nothing, but the plan and the tracked states are the same.
* **Load/store.** Per raster attachment: `Clear` if declared, `DontCare` if fully overwritten or a transient with no earlier writer, otherwise `Load`. `Store` if the next live pass touching the subresource uses its contents or the texture is imported, otherwise `DontCare`. Each choice carries its reason.

`Execute()` records each live pass: it writes the planned states through to the images', buffers' and acceleration structures' trackers, so foreign code and the next frame start from them, then records the barrier batch and the pass.

## Renderers

A renderer builds one graph per frame from the `RenderFramework`'s texture pool, which outlives renderer recreation so a pipeline switch reuses the images both pipelines' graphs need (`pipeline_switch_pool` checks it), and hands it to `Renderer::ExecuteGraph`, which compiles it, writes its dump when one is requested, and records it. Every renderer ends its frame with `Renderer::AddPostChain(graph, scene, screen_pass)`, whose passes are Raster passes unless stated:

| Pass | Declares |
| --- | --- |
| the screen pass (`ToneMapping`, `OutputImage` or `Upsample`), when given | `Sampled` scene, `ColorWrite` Screen (fully overwritten) |
| `Readback` (screenshot without UI) | Copy pass: `CopySrc` Screen, `CopyDst` ScreenshotBuffer, a staging buffer created with the pass (its size comes from `RenderGraph::GetFormat` and `GetSize`) and saved once the frame completes |
| `Ui` (UI shown, not headless) | `ColorWrite` Screen, `NativeAccess()` for ImGui |
| `Readback` (screenshot with UI) | as above |
| `Present` | `Sampled` Screen, `ColorWrite` BackBuffer (fully overwritten) |

Screen is a transient at output resolution whose format and sampler the renderer chooses once (`Renderer::InitPostChain`): `B8G8R8A8Srgb` with nearest sampling for the renderers that tone map on the GPU, the CPU renderer's `RGBAFloat16` with bilinear sampling otherwise. Without a screen pass, `scene` is the screen. The screen passes and `Present` are `ScreenQuadPass`es built from their output format; each samples its input with the sampler the input's image carries, and `Present` applies the window's pre-rotation. `Ui` draws into the rendering the graph begins over Screen, and the ImGui backend compiles its pipelines for Screen's format (`RHIUiHandler::Setup` takes an attachment signature).

The CPU renderer runs on the graph. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `Upload` | Copy | `CopySrc` HostSceneColor, the host buffer the path tracer filled; `CopyDst` SceneColor (fully overwritten) |
| `Upsample` (`render_scale` < 1), `Readback`, `Ui`, `Present` | | the post chain; without upsampling SceneColor is the screen |

SceneColor is a transient at scene resolution, already tone-mapped on the CPU, sampled bilinearly so the upsampling filters.

The GPU renderer runs on the graph. The accumulator, the TLAS, the path-tracing denoiser inputs (once a denoiser has needed them) and the denoiser's output (and NRD's output history) are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `ClearAccumulator` (camera moved or scene changed) | Raster | `ColorWrite` Accumulator, cleared to zero; no draws |
| `BuildTLAS` (primitives changed) | Copy | `AccelerationStructureBuild` TLAS |
| `PathTrace` (accumulating) | Compute | `AccelerationStructureRead` TLAS; `StorageReadWrite` Accumulator; `StorageWrite` of the six denoiser inputs once allocated, because the tracer binds them as storage images on every dispatch (dummies until then); each declaration binds its image to the tracer |
| `Nrd` (NRD encodes) | External | `Sampled` (compute) of the six inputs and Accumulator; `StorageWrite` NrdOutput, `StorageReadWrite` NrdOutputHistory |
| `MetalFx` (MetalFX encodes) | External | `Sampled` (compute) of the four auxiliary inputs and Accumulator; `StorageWrite` of the displayed image: MetalFxOutput (the scaler's output) before the handoff starts, MetalFxResolvedOutput after |
| `ToneMapping`, `Readback`, `Ui`, `Present` | | the post chain from the displayed texture |

`GPURenderer::Update` stages the primitive changes on the TLAS: `RHITLAS::Build` and `Update` build dirty BLAS in their own submits (Metal compacts them, committing the frame's command buffer and waiting), write the instances and allocate the structure, and `BuildTLAS` records the staged build or refit. `PathTrace` runs on the renderer's timed `RHIComputePass`, which dynamic spp reads back. The displayed texture is the provider's output when it encodes this frame, the Accumulator when a frame traces without denoising, and otherwise whatever tone mapping displayed last (a converged or paused frame keeps the last denoised output, imported as DenoiserOutput). A denoiser's pass keeps its internal textures private: NRD's pool and `IN_*`/`OUT_*` textures and MetalFX's prepared textures (and its scaler output while it resolves) keep their own transitions, and every image the pass declares ends in its declared state.

The Forward renderer runs on the graph. The directional shadow map, scene color and scene depth are transients; the IBL maps and the sky map are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `DirectionalShadow` (directional light) | Raster | `DepthWrite` ShadowMap (`shadow_map_resolution` squared), cleared |
| `BasePass` | Raster | `Sampled` ShadowMap and each ready IBL map (IblBrdf, IblDiffuse, IblSpecular); `ColorWrite` SceneColor and `DepthWrite` SceneDepth, both cleared |
| `SkyBox` (sky map) | Raster | `Sampled` of the sky map it draws (an IBL map in the IBL map output modes); `ColorWrite` SceneColor, `DepthTest` SceneDepth |
| `ToneMapping`, `Readback`, `Ui`, `Present` | | the post chain from SceneColor; the `IBLBrdfTexture` output mode draws the BRDF map in `OutputImage` instead of `ToneMapping`, and the scene passes are culled |

Tone mapping upsamples when `render_scale` < 1, so the graph has the same passes at any scale; the scene color then carries a bilinear, edge-clamped sampler (`Renderer::GetSceneColorDesc`). The renderer passes each pass its inputs every frame: the shadow map and IBL maps as `LightingInputs`, where a missing one (no directional light, a map still cooking) samples a placeholder, and the sky box's map and the output image as the output mode selects them (`Renderer::GetSkyBoxMap`, `GetOutputImage`). IBL maps are imported only once ready. While a map cooks on the GPU, `ImageBasedLighting::AddCookPasses` starts the frame's graph with one cook step per map still cooking:

| Pass | Kind | Declares |
| --- | --- | --- |
| `ClearIblBrdfCook`, `ClearIblDiffuseCook`, `ClearIblSpecularCook` (first step, once per subresource) | Raster | `ColorWrite` of one mip of one layer of the map being cooked, cleared; no draws |
| `CookIblBrdf`, `CookIblDiffuse` | Compute | `StorageReadWrite` IblBrdfCook or IblDiffuseCook (all six faces as a 2D array), bound to the cook shader |
| `CookIblSpecular` | Compute | `StorageReadWrite` of the mip of IblSpecularCook being cooked |

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

`Dump()` returns the compiled graph as JSON: passes (kind, culled with reason, accesses, barriers with layouts and accesses, attachments with load/store and reasons, each naming its subresources unless it covers every one) and resources (kind, format, size class, first and last use, usage, physical image). Buffers and acceleration structures follow the textures; their barriers have no layouts, and their usage is the union of their accesses. It names size classes instead of pixel sizes, so a graph dumps the same at any resolution and on either backend.

## Tests

`render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans, executes them and reads results back, including a screen quad whose pipeline was created from an attachment signature with nothing bound and draws the texture and sampler its pass declared, a texture copied through a buffer whose memory barrier orders the copies, and accesses to subresources of a cube map with the barriers and store actions they plan per subresource. `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)).

Each renderer on the graph has a golden graph shape in [tests/render_graph/golden/](../tests/render_graph/golden/) (`<pipeline>.txt`), checked by a `<pipeline>_graph_shape` registry case. The `render_graph_dump` test case waits until the scene is ready for a screenshot and asks the renderer (`RenderFramework::RequestGraphDump`) to write the next graph it executes to `screenshots/render_graph.json`, which the device runners pull like screenshots. [tests/render_graph/graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, attachment and resource and prints a unified diff against the golden; `--update` rewrites the golden from the dump. A converged GPU frame traces nothing, so `gpu_graph_shape` runs `render_graph_dump_accumulating`, which asks for the dump as soon as the scene is loaded, while the accumulator still converges; the golden is a frame without a denoiser, clear or screenshot, and needs ray query support (lavapipe has it). The forward and deferred goldens are frames with a directional light, a sky map and ready IBL maps. Tests run headless, so the goldens show no `Ui` pass, and the back buffer's first barrier has no present access to wait for.
