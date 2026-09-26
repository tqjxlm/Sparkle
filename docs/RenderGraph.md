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

* **Textures.** `CreateTexture` makes a transient: single-sampled, one mip, with a size class resolved from `RenderResolution` (`Scene`, `Output`, or `Absolute` with an explicit size). Its image usage is the union of its declared accesses. `Import` brings in a persistent image (history, swap chain, IBL maps); a graph imports each image once.
* **Handles.** `RGTexture` is a plain index. Passes run in declaration order, and every access depends on the last write before it in that order, so handles need no versions.
* **Accesses.** `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `StorageRead`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`. Shader accesses take an optional stage mask; the default is the pass kind's stage (raster: pixel, compute: compute, external: all). A pass declares each texture once. `FullyOverwrites()` states that the pass writes every texel, which discards previous contents; `SideEffect()` keeps a pass whose outputs nothing reads (readback, present).
* **Pass kinds.** Each kind's execute function receives a context that exposes only what the kind may record:

| Kind | Context records | The graph around it |
| --- | --- | --- |
| Raster | draws | begins and ends rendering over the declared attachments; the opening barriers sit inside the pass's debug label |
| Compute | dispatches | brackets it with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | image-to-buffer copies | records the barriers before it |
| External | anything, through the raw `RHICommandContext` | records the barriers before it, then checks the contract below |

`ctx.GetImage(texture)` returns the image behind a texture the pass declared. An External pass must leave every declared image in its declared layout, with no pending access beyond the declared one; foreign code that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## Compilation

`Compile()` records nothing. Declaration errors abort in every build, including Release: an invalid or doubly declared handle, an access the pass kind may not declare, two attachments in one slot, a raster pass without attachments, a transient read before any pass writes it, attachments of different sizes, an import lacking the usages its accesses need, and a broken External contract.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import, or writes contents a later live pass uses. A write uses the previous contents unless it clears or its pass fully overwrites. Culled passes are dumped with their unread outputs. `render_graph_cull` (default `true`) turns culling off.
* **Transients.** Transients whose lifetimes (first to last live pass) do not overlap share one image when format, extent and sampler match, in order of first use, so a graph of the same shape maps each transient to the same image every frame. Images come from an `RGTexturePool`, which the owner of the graph keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState`, for imports and transients alike, and applies `TransitionImageState` per access. A pooled image reused from the previous frame therefore waits for that frame's last access, and a swap chain image's first attachment write chains to the acquire wait because attachment writes add their own access to the barrier source. A write discards the contents (`Undefined` source layout) when it clears, fully overwrites, or writes a transient no earlier pass wrote. Barriers are batched per pass. Depth tests synchronize as depth writes, because the attachment store op writes the depth image. On Metal the batches record nothing, but the plan and the tracked states are the same.
* **Load/store.** Per raster attachment: `Clear` if declared, `DontCare` if fully overwritten or a transient with no earlier writer, otherwise `Load`. `Store` if the next live pass touching the texture uses its contents or the texture is imported, otherwise `DontCare`. Each choice carries its reason.

`Execute()` records each live pass: it writes the planned states through to the images' trackers, so foreign code and the next frame start from them, then records the barrier batch and the pass.

## Dump

`Dump()` returns the compiled graph as JSON: passes (kind, culled with reason, accesses, barriers with layouts and accesses, attachments with load/store and reasons) and resources (kind, format, size class, first and last use, usage, physical image). It names size classes instead of pixel sizes, so a graph dumps the same at any resolution and on either backend.

## Tests

`render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans, executes them and reads one result back. `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)).
