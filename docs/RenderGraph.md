# Render Graph

The render graph ([libraries/include/renderer/graph/RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h)) records one frame's GPU passes from declared accesses. Passes state what they read and write; the graph derives culling, transient images, image layouts, barriers and attachment load/store actions, and can dump every decision it made.

## Building a Graph

A graph lives for one frame: build it, `Compile()`, `Execute(command_context)`, then destroy it.

```cpp
RenderGraph graph(texture_pool, render_config);
auto scene_color = graph.CreateTexture("SceneColor", {.format = PixelFormat::RGBAFloat16, .size_class = RGSizeClass::Scene});
auto history = graph.Import("History", history_image);

graph.AddRasterPass("Lighting", [&](RGBuilder &b) {
    b.Sampled(history);
    b.ColorWrite(scene_color, 0, Vector4(0, 0, 0, 1));
    return [this](RGRasterContext &ctx) { ctx.DrawMesh(pso_, draw_args_); };
});

graph.Compile();
graph.Execute(*rhi->GetCommandContext());
```

* **Textures.** `CreateTexture` makes a transient: single-sampled, one mip, with a size class resolved from `RenderResolution` (`Scene`, `Output`, or `Absolute` with an explicit size). Its image usage is the union of its declared accesses. `Import` brings in a persistent image (history, swap chain, IBL maps); importing an image again returns the texture, and name, of its first import, so passes that sample the same image need not coordinate.
* **Handles.** `RGTexture` is a plain index. Passes run in declaration order, and every access depends on the last write before it in that order, so handles need no versions.
* **Accesses.** `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `StorageRead`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`. Shader accesses take an optional stage mask; the default is the pass kind's stage (raster: pixel, compute: compute, external: all). A pass declares each texture once. `FullyOverwrites()` states that the pass writes every texel, which discards previous contents; `SideEffect()` keeps a pass whose outputs nothing reads (readback, present).
* **Pass kinds.** Each kind's execute function receives a context that exposes only what the kind may record:

| Kind | Context records | The graph around it |
| --- | --- | --- |
| Raster | draws | begins and ends rendering over the declared attachments; the opening barriers sit inside the pass's debug label |
| Compute | dispatches | brackets it with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | copies between images and buffers | records the barriers before it |
| External | anything, through the raw `RHICommandContext` | records the barriers before it, then checks the contract below |

`ctx.GetImage(texture)` returns the image behind a texture the pass declared. An External pass must leave every declared image in its declared layout, with no pending access beyond the declared one; foreign code that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## Compilation

`Compile()` records nothing. Declaration errors abort in every build, including Release: an invalid or doubly declared handle, an access the pass kind may not declare, two attachments in one slot, a raster pass without attachments, a transient read before any pass writes it, attachments of different sizes, an import lacking the usages its accesses need, and a broken External contract.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import, or writes contents a later live pass uses. A write uses the previous contents unless it clears or its pass fully overwrites. Culled passes are dumped with their unread outputs. `render_graph_cull` (default `true`) turns culling off.
* **Transients.** Transients whose lifetimes (first to last live pass) do not overlap share one image when format, extent and sampler match, in order of first use, so a graph of the same shape maps each transient to the same image every frame. Images come from an `RGTexturePool`, which the owner of the graph keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState`, for imports and transients alike, and applies `TransitionImageState` per access. A pooled image reused from the previous frame therefore waits for that frame's last access, and a swap chain image's first attachment write chains to the acquire wait because attachment writes add their own access to the barrier source. A write discards the contents (`Undefined` source layout) when it clears, fully overwrites, or writes a transient no earlier pass wrote. Barriers are batched per pass. Depth tests synchronize as depth writes, because the attachment store op writes the depth image. On Metal the batches record nothing, but the plan and the tracked states are the same.
* **Load/store.** Per raster attachment: `Clear` if declared, `DontCare` if fully overwritten or a transient with no earlier writer, otherwise `Load`. `Store` if the next live pass touching the texture uses its contents or the texture is imported, otherwise `DontCare`. Each choice carries its reason.

`Execute()` records each live pass: it writes the planned states through to the images' trackers, so foreign code and the next frame start from them, then records the barrier batch and the pass.

## Renderers

A renderer builds one graph per frame from its `graph_texture_pool_` and hands it to `Renderer::ExecuteGraph`, which compiles it, writes its dump when one is requested, and records it. `Renderer::AddReadback` adds the screenshot readback: when a screenshot is pending, a `Readback` Copy pass with `SideEffect()` copies the texture into a staging buffer that is saved once the frame completes. `Renderer::AddPresentPasses` adds the tail every renderer shares once its screen texture holds the final image: the readback without UI, `Ui` when the UI is shown, the readback with UI, and `Present` into the imported back buffer.

The CPU renderer runs on the graph. Its screen texture, the composite target (only when `render_scale` < 1) and the back buffer are imports, because the legacy passes bind them at construction. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `Upload` | Copy | `CopyDst` Screen (fully overwritten) from the host buffer the path tracer filled |
| `Upsample` (`render_scale` < 1) | External | `Sampled` Screen, `ColorWrite` Composite (fully overwritten) |
| `Readback` (screenshot without UI) | Copy | `CopySrc` Composite |
| `Ui` (UI shown) | External | `ColorWrite` Composite |
| `Readback` (screenshot with UI) | Copy | `CopySrc` Composite |
| `Present` | External | `Sampled` Composite, `ColorWrite` BackBuffer (fully overwritten) |

Without upsampling, Composite is the Screen texture. The External passes wrap legacy `ScreenQuadPass` and `UiPass` render passes, whose final layouts are their attachment layouts, so every transition between them comes from the graph.

The GPU renderer runs on the graph. The accumulator, the path-tracing denoiser inputs (once a denoiser has needed them), the denoiser's output (and NRD's output history), the tone-mapped screen texture and the back buffer are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `ClearAccumulator` (camera moved or scene changed) | Raster | `ColorWrite` Accumulator, cleared to zero; no draws |
| `PathTrace` (accumulating) | Compute | `StorageReadWrite` Accumulator; `StorageWrite` of the six denoiser inputs once allocated, because the tracer binds them as storage images on every dispatch |
| `Nrd` (NRD encodes) | External | `Sampled` (compute) of the six inputs and Accumulator; `StorageWrite` NrdOutput, `StorageReadWrite` NrdOutputHistory |
| `MetalFx` (MetalFX encodes) | External | `Sampled` (compute) of the four auxiliary inputs and Accumulator; `StorageWrite` of the displayed image: MetalFxOutput (the scaler's output) before the handoff starts, MetalFxResolvedOutput after |
| `ToneMapping` | External | `Sampled` of the displayed texture, `ColorWrite` Screen (fully overwritten) |
| `Readback`, `Ui`, `Present` | | as `Renderer::AddPresentPasses` adds them |

`PathTrace` runs on the renderer's timed `RHIComputePass`, which dynamic spp reads back. The displayed texture is the provider's output when it encodes this frame, the Accumulator when a frame traces without denoising, and otherwise whatever tone mapping displayed last (a converged or paused frame keeps the last denoised output, imported as DenoiserOutput). A denoiser's pass keeps its internal textures private: NRD's pool and `IN_*`/`OUT_*` textures and MetalFX's prepared textures (and its scaler output while it resolves) keep their own transitions, and every image the pass declares ends in its declared state.

The Forward renderer runs on the graph. Its legacy passes bind their images at construction, so the scene color and depth, the directional shadow map, the IBL maps, the sky map, the tone-mapped screen texture and the back buffer are imports. Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `DirectionalShadow` (directional light) | External | `DepthWrite` ShadowMap, cleared |
| `BasePass` | External | `Sampled` of ShadowMap and of each ready IBL map (IblBrdf, IblDiffuse, IblSpecular); `ColorWrite` SceneColor and `DepthWrite` SceneDepth, both cleared |
| `SkyBox` (sky map) | External | `Sampled` of the sky map the pass draws (an IBL map in the IBL map output modes); `ColorWrite` SceneColor, `DepthTest` SceneDepth |
| `ToneMapping`, or `OutputImage` (`IBLBrdfTexture` output) | External | `Sampled` SceneColor (OutputImage: the BRDF map), `ColorWrite` Screen (fully overwritten) |
| `Readback`, `Ui`, `Present` | | as `Renderer::AddPresentPasses` adds them |

Tone mapping upsamples when `render_scale` < 1, so the graph has the same passes at any scale. IBL maps are imported only once ready: cook dispatches record during `Tick`, outside the graph, into images the graph never imports, and a finished map leaves its cook with every subresource in one tracked state.

The Deferred renderer runs on the graph with the same imports as Forward, plus the packed GBuffer. It shares the Forward renderer's shadow, IBL, sky box and tone mapping wrappers (`Renderer::AddDirectionalShadowPass`, `ImportIblMaps`, `AddSkyBoxPass`, `AddToneMappingPass`). Each frame:

| Pass | Kind | Declares |
| --- | --- | --- |
| `DirectionalShadow` (directional light) | External | `DepthWrite` ShadowMap, cleared |
| `GBuffer` | External | `ColorWrite` GBufferPacked and `DepthWrite` SceneDepth, both cleared |
| `Lighting` | External | `Sampled` of GBufferPacked, SceneDepth, ShadowMap and each ready IBL map; `ColorWrite` SceneColor (fully overwritten) |
| `SkyBox` (sky map) | External | as in Forward: `Sampled` sky map, `ColorWrite` SceneColor, `DepthTest` SceneDepth |
| `ToneMapping`, or `OutputImage` (`IBLBrdfTexture` output) | External | as in Forward |
| `Readback`, `Ui`, `Present` | | as `Renderer::AddPresentPasses` adds them |

Lighting reconstructs world positions from the scene depth it samples, and the sky box then depth-tests against it, so the graph moves SceneDepth from the depth attachment layout to `Read` and back within the frame.

## Dump

`Dump()` returns the compiled graph as JSON: passes (kind, culled with reason, accesses, barriers with layouts and accesses, attachments with load/store and reasons) and resources (kind, format, size class, first and last use, usage, physical image). It names size classes instead of pixel sizes, so a graph dumps the same at any resolution and on either backend.

## Tests

`render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans, executes them and reads one result back. `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)).

Each renderer on the graph has a golden graph shape in [tests/render_graph/golden/](../tests/render_graph/golden/) (`<pipeline>.txt`), checked by a `<pipeline>_graph_shape` registry case. The `render_graph_dump` test case waits until the scene is ready for a screenshot and asks the renderer (`RenderFramework::RequestGraphDump`) to write the next graph it executes to `screenshots/render_graph.json`, which the device runners pull like screenshots. [tests/render_graph/graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, attachment and resource and prints a unified diff against the golden; `--update` rewrites the golden from the dump. A converged GPU frame traces nothing, so `gpu_graph_shape` runs `render_graph_dump_accumulating`, which asks for the dump as soon as the scene is loaded, while the accumulator still converges; the golden is a frame without a denoiser, clear or screenshot, and needs ray query support (lavapipe has it). The forward and deferred goldens are frames with a directional light, a sky map and ready IBL maps. Tests run headless, so the goldens show no `Ui` pass, and the back buffer's first barrier has no present access to wait for.
