# Render Graph

The render graph ([libraries/include/renderer/graph/RenderGraph.h](../libraries/include/renderer/graph/RenderGraph.h)) records one frame's GPU passes from the accesses they declare to textures, buffers and acceleration structures. From these declarations it derives which passes run, which consecutive raster passes share one render pass, which reads stay pixel-local within one, which images back the transient textures and which of them stay in tile memory, and the image layouts, barriers and attachment load/store actions, and it can dump every decision with its reason. A graph lives for one frame: it is built, compiled, executed and destroyed, and every renderer records its frame through one.

The RHI only lowers the graph's plan: `RHICommandContext::BeginRendering` over an `RHIRenderingInfo` (attachments already in the layouts it gives, with load/store actions and clear values), access-based barriers, `PixelLocalBarrier` between draws of one rendering, and pipelines compiled per attachment signature. RHI tests ([tests/rhi/](../tests/rhi/)) record through these primitives directly.

## Adding a Pass to a Renderer

`Renderer::Render(dump)` creates the frame's graph from the texture pool the renderer was given, lets the renderer add its scene passes (`BuildGraph`, which returns the texture they leave the scene in), adds the post chain (see [Renderers](#renderers)), then compiles the graph, executes it with the renderer's pass timers and returns its dump when `dump` is true. A pass class keeps its persistent state (pipelines, shaders, uniform buffers) and adds its pass through an `AddTo(graph, inputs...)` method. The setup lambda declares the pass's accesses on the builder and returns the lambda that records the pass.

```cpp
RGTexture MyRenderer::BuildGraph(RenderGraph &graph)
{
    const auto history = graph.Import("History", history_image_);
    const auto scene_color = graph.CreateTexture("SceneColor", SceneColorDesc);

    graph.AddRasterPass("Lighting", [this, history, scene_color](RGBuilder &builder) {
        using Table = LightingPixelShader::ResourceTable;
        builder.Sampled(history, &Table::history, &Table::historySampler, HistorySampler);
        builder.ColorWrite(scene_color, ColorSlot::SceneColor, Vector4(0, 0, 0, 1));
        return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
    });

    return scene_color;
}
```

Outside a renderer, as in the tests, the owner of a graph calls `Compile()` and `Execute(command_context)` itself, and destroys each graph before its texture pool.

### Reading Pixel-Locally

* A pass that reads what an earlier pass of its physical pass wrote at the same pixel declares `PixelLocalRead(texture, slot, binding, sampler_binding, sampler)`, or `PixelLocalRead(texture, slot, binding)` when the base variant loads the texture at its pixel, and has two shader variants sharing one `ResourceTable`: the base variant samples or loads the texture, a variant compiled with a define reads it pixel-locally. Its record function draws with the pipeline of the variant `context.IsPixelLocal(texture)` selects. `ToneMappingPass` (sampled) and `DirectionalLightingPass` (loaded) are the examples.
* A variant is a `<source>:<DEFINE>` entry of `SHADER_VARIANTS` in [shaders/CMakeLists.txt](../shaders/CMakeLists.txt). The shader build compiles the source with `-D<DEFINE>=1` into `<source>.<DEFINE>.spv` and `.metal`, and `RHIContext::CreateShader<T>("<DEFINE>")` loads it.
* In the variant, the input is a global `SubpassInput` with `[[vk::input_attachment_index(slot)]]` under the name and binding of the sampled texture, read with `SubpassLoad()` in the fragment entry point. Slang's Metal target rejects a `SubpassInput` in a `ParameterBlock` and needs `[ForceInline]` on a helper that reads one.
* Vulkan reads the input as an input attachment of the same index as its color attachment; Metal as a `[[color(slot)]]` framebuffer fetch, an entry point argument without a binding. Reflection retypes the texture's binding as `InputAttachment` in the variant's table: an `INPUT_ATTACHMENT` descriptor in `RENDERING_LOCAL_READ` on Vulkan, nothing to bind on Metal. A variant does not warn about the bindings its define compiles out, such as the sampler.
* The pass compiles its variant pipeline only once the graph keeps the read pixel-local, so a device without pixel-local reads never compiles a shader it cannot run (Metal compiles MSL at load).
* Check the cooked shaders, not the source attributes: the variant's SPIR-V decorates the input with `InputAttachmentIndex <slot>`, and its MSL takes it as `[[color(<slot>)]]`.

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

* Textures: `ColorWrite(slot, clear)`, `DepthWrite(clear)`, `DepthTest`, `Sampled`, `PixelLocalRead(slot)`, `StorageWrite`, `StorageReadWrite`, `CopySrc`, `CopyDst`.
* Buffers: `CopySrc`, `CopyDst`.
* Acceleration structures: `AccelerationStructureBuild` (a build or refit) and `AccelerationStructureRead` (ray queries).
* An attachment (`ColorWrite`, `DepthWrite`, `DepthTest`) is one subresource. A color slot is the fragment output location.
* `PixelLocalRead(slot)` reads, in the pixel stage, the value an earlier member of the pass's physical pass wrote to color slot `slot` at the same pixel. It is a request, which the compiler may lower to `Sampled` (see [Pixel-local reads](#what-the-compiler-decides)); `context.IsPixelLocal(texture)` tells the record function which one it records. Only Raster passes declare it, never at a slot the pass writes.
* Shader accesses take an optional stage mask; the default is the pass kind's stage (Raster: pixel, Compute: compute, External: all).
* A pass declares each texture subresource, buffer and acceleration structure once.

### Pass Flags

* `FullyOverwrites()`: the pass writes every texel of the textures it writes, which discards the previous contents of those it does not also read.
* `SideEffect()`: the pass runs even when nothing reads its outputs.
* `NativeAccess()`: a Raster pass records foreign commands (ImGui) through the raw command context (`GetNativeContext()`), inside the rendering of its physical pass, which it joins like any other Raster pass. The foreign code resets the backend state it records around.

### Bindings

* A shader access may name the binding member of a shader's `ResourceTable` that reads the resource, e.g. `&ToneMappingPixelShader::ResourceTable::screenTexture`: a `Texture2D` member for `Sampled`, a `StorageImage2D` member for the storage accesses, an `AccelerationStructure` member for `AccelerationStructureRead`.
* While the pass records, every pipeline drawn or dispatched through the command context binds the resource there, in each of its resource tables that has the member, so each input is declared once and serves every pipeline of the pass that uses the table.
* A sampled binding binds the image's default view of every subresource. A storage binding binds a view of its access's single mip, with a cube's layers as a 2D array. Views are created at compile.
* A `PixelLocalRead` names a `Texture2D` member and, optionally, a `Sampler` member with sampler attributes, like a `Sampled` access: kept pixel-local, it binds only the texture, which a shader variant reading it pixel-locally declares as an input attachment (see [Reading Pixel-Locally](#reading-pixel-locally)); lowered, it binds both, as `Sampled` does.
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
| Raster | `AddRasterPass` | draws; with `NativeAccess()`, anything the open rendering allows | records the pass inside the rendering of its [physical pass](#what-the-compiler-decides), whose opening barriers sit inside the physical pass's debug label and timer |
| Compute | `AddComputePass`, with an `RHIComputePass` | dispatches | brackets the pass with the given `RHIComputePass`, whose label and timer cover the barriers |
| Copy | `AddCopyPass` | copies between images and buffers; acceleration structure builds (`BuildAccelerationStructure` records the build or refit staged on the structure) | records the barriers before it, inside the pass's debug label |
| External | `AddExternalPass` | anything, through the raw `RHICommandContext` | records the barriers before it, inside the pass's debug label |

* `context.GetImage(texture)` returns the image behind a texture the pass declared. A Copy pass reaches buffers and acceleration structures only through its copy and build commands.
* Copies record no barrier of their own: the graph orders them through the declared accesses, and makes the data of a buffer the host reads visible to it.
* Every pass must leave each declared image in its declared layout, with no pending access beyond the declared one; the graph checks this after the pass records.
* A pass records only through its context, also when it transitions or uploads an image (`RHIImage::Transition` and `Upload` take the command context). `RHIContext::GetCommandContext()` is for code outside graph passes and asserts while a graph executes.
* Foreign code in an External pass that still calls `RHIImage::Transition` sees the state the graph planned, because the graph writes it to the image before the pass runs.

## What the Compiler Decides

`Compile()` validates the declarations and plans the frame without recording anything. The [dump](#dump-format) shows every decision; culling, physical passes and load/store carry their reasons in the dump's `cull_reason`, `break_reason`, `load_reason` and `store_reason` fields.

* **Culling.** Walking backwards, a pass lives if it has a side effect, writes an import (every buffer and acceleration structure is one), or writes contents a later live pass uses. A read uses the contents earlier passes left. A write that does not clear, in a pass that does not fully overwrite, passes them on only when a later live pass uses its result. A culled pass stays in the dump, with a `cull_reason` naming its unread outputs.
* **Physical passes.** Consecutive live Raster passes, in declaration order, form one physical pass, recorded as one rendering over the union of their attachments. Every other live pass is a physical pass of its own. The next live pass joins the current physical pass unless it breaks one of these rules, listed in order; the dump lists every rule it breaks, with the resources each names, as the physical pass's `breaks`, and the first one as its `break_reason`, with the first resource it names as `break_resource`. A pass of another kind than Raster breaks only `ExternalPass` or `NonRasterPass`:
  * `ExternalPass` or `NonRasterPass`: either pass is an External pass, or not a Raster pass.
  * `TargetSizeMismatch`: the attachments differ in size.
  * `DifferentDepth(res)`: the next pass attaches another depth subresource than a member. A pass without a depth attachment may join a physical pass with one, and the reverse.
  * `SlotConflict(res)`: the next pass attaches a subresource at a slot where a member attaches another one, or at another slot than a member.
  * `ClearInPass(res)`: the next pass clears a subresource a member attaches; a load op applies only where the rendering begins.
  * `SlotConflict(res)` also: the next pass reads pixel-locally, at another slot, a subresource a member attaches.
  * `NonLocalRead(res)`: a shader access of the next pass depends on an access of the physical pass: it samples or stores what a member wrote, stores what a member read, or samples it in a shader stage no member sampled it in. Its barrier would have to wait inside the rendering. A `PixelLocalRead` of what a member attaches at the slot it reads is exempt: its barrier waits inside the rendering by design.
  * `AttachmentReadInPass(res)`: the next pass attaches a subresource a member sampled, stored or read pixel-locally, which one layout for the whole rendering cannot serve, or whose read a later attachment write would race.
  * `TileBudget`: with `render_graph_tile_budget_split` on, the color attachments of the members and the next pass together exceed the [tile budget](#what-the-compiler-decides).
  * `NoPixelLocalSupport(res)`: the next pass reads pixel-locally what a member attaches, but `render_graph_pixel_local` is off or the device has no pixel-local reads (`RHIContext::SupportsPixelLocalRead`).
  * `Disabled`: `render_graph_merge` is off.
  * No rule limits the attachment count: every color slot is below `MaxNumColorAttachments` and holds one subresource per physical pass.
* **Tile budget.** Each raster physical pass reports the bytes per pixel of its color attachments (`color_bytes_per_pixel`) against the device's tile budget (`RHIContext::GetTileBudget`), the color bytes per pixel one render pass keeps in tile memory at the full tile size, or `render_graph_tile_budget` when set:
  * Apple GPUs, from the Metal feature set tables' maximum implicit image block size per pixel: 32 B on Apple2 and Apple3, 64 B on Apple4 to Apple6, 128 B from Apple7, which no attachment set reaches (8 slots of at most 16 B).
  * Mali and Immortalis GPUs, from the Arm GPU datasheets' tile bits per pixel: 32 B (256 bits) from Mali-G72 on. The theoretical limit is higher from Mali-G710 on, but 256 bits keep the full 16×16 tile.
  * Unknown elsewhere, with no budget: Adreno (whose tile size the driver chooses from its tile memory) and desktop GPUs, MoltenVK included.
  * Over the budget, a tiler shrinks its tiles rather than spilling, which usually still beats storing and reloading attachments, so the physical pass only warns (`over_tile_budget` in the dump, and in the opportunity report). `render_graph_tile_budget_split` turns the budget into the hard `TileBudget` break for on-device A/B measurements; with `render_graph_tile_budget` it splits on devices without a known budget too. Depth attachments do not count.
* **Pixel-local reads.** A `PixelLocalRead` of what an earlier member of its physical pass attaches at the slot it reads stays pixel-local: it becomes a `PixelLocalRead` access, and every attachment of that subresource in the physical pass takes the `LocalRead` layout (Vulkan `RENDERING_LOCAL_READ`). Any other request is lowered to `Sampled`, with the dump's `lowered_reason`: the break reason of the physical pass of the texture's last writer, e.g. `TargetSizeMismatch` when tone mapping upsamples or `NonRasterPass` after a Compute writer, or `NoWriter` when no pass of the graph writes it. The device decides only through `NoPixelLocalSupport`.
* **Steps and signatures.** Within a physical pass, consecutive members attaching the same subresources at the same slots share a step (the dump's `step`), unless the later one reads pixel-locally. A member draws against the physical pass's attachment signature, with a color write mask of 0 on the slots it does not attach, and neither tests nor writes the depth attachment when it has none. The write masks need Vulkan's `independentBlend` feature, which device selection requires.
* **Memoryless.** A transient that only attachments (`ColorWrite`, `DepthWrite`, `DepthTest`) and pixel-local reads of one raster physical pass use is memoryless: its contents never leave tile memory, because no pass outside the physical pass reads them. A texture `render_graph_view` shows, or a readback copies, is therefore never memoryless. Its load op is `Clear` or `DontCare` and its store op `DontCare`, which the compiler checks. It gets a memoryless image where `RHIContext::SupportsMemorylessImage(format, usages)`: `MTLStorageModeMemoryless` on Apple family GPUs outside the iOS simulator, whose depth textures must be private, and `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` in lazily allocated memory on Vulkan devices whose lazily allocated memory type can back such an image (MoltenVK and mobile GPUs, not desktop GPUs or lavapipe; MoltenVK backs no image it reads as an input attachment with one). Elsewhere it gets an ordinary pooled image, as does a transient used before and after a pixel-local barrier on a device that loses memoryless contents there (`RHIContext::KeepsMemorylessAcrossPixelLocalBarrier`): MoltenVK ends and restarts the Metal render pass at a pixel-local barrier, storing and reloading the attachments. The dump's `memoryless` is the compiler's decision, the same on every device, and `backing` the image the device gave.
* **Aliasing.** Transients whose lifetimes (first to last live pass) do not overlap in physical passes share one image when format, pixel size and memorylessness match, assigned in order of first use, so a graph of the same shape maps each transient to the same image every frame. Two transients one physical pass attaches never share one. The dump's `physical` names each transient's image, and `first_use` and `last_use` its lifetime.
* **Pooling.** Images come from an `RGTexturePool`, which the owner of the graphs keeps across frames. The pool serves one graph at a time and hands the same images to the next graph immediately; images unused for `RGTexturePool::UnusedGraphsBeforeRelease` graphs go through deferred deletion. A pooled image serves a request whose usages it covers and whose memory properties it has, so a memoryless image never backs a stored transient, nor the reverse. A pooled image keeps the debug name of the transient it was created for; the dump's `physical` tells which transients it backs.
* **Barriers.** Planning starts from each physical image's tracked `RHIImageState` per subresource, for imports and transients alike, and the dump lists each pass's barriers with their layouts and accesses:
  * Each access moves its subresources into its layout and access with one barrier per run of mips in one state, spanning every layer when each mip's layers share a state, otherwise within each layer.
  * A pooled image reused from the previous frame waits for that frame's last access.
  * A swap chain image's tracked access is `Present` from its creation and after each frame, so its first barrier waits for its acquire: on Vulkan a barrier from `Present` starts at the stage where the frame's submit waits for the acquired image.
  * A write discards the contents (`Undefined` source layout) when it clears, fully overwrites without reading, or writes a transient no earlier pass wrote.
  * Depth tests synchronize as depth writes, because the attachment's store op writes the depth image. Likewise a pixel-local read leaves the color write of its attachment's store op pending.
  * Buffers and acceleration structures synchronize through memory barriers by the same rule without layouts, starting from their tracked access (`RHITrackedAccess`).
  * A build also waits for earlier builds, which covers the BLAS it reads (built before the frame) and the scratch memory it reuses.
  * Right after its last live pass, a buffer the host reads moves to `HostRead` through a barrier from its last access (the pass's `barriers_after` in the dump), unless it has none: waiting for the device does not make device writes visible to the host. A buffer no live pass uses gets no barrier and keeps its tracked access.
  * An attachment a member attaches after an earlier member of its physical pass gets no barrier: rasterization order orders attachment accesses within one rendering.
  * Barriers are batched per physical pass and recorded before its rendering; the dump lists each barrier with the member that needs it. The physical pass rules leave each subresource at most one barrier per batch. On Metal the batches record nothing, but the plan and the tracked states are the same.
  * The barrier of a pixel-local read (`ColorWrite` → `PixelLocalRead` in `LocalRead`) is recorded inside the rendering, right before its member, through `RHICommandContext::PixelLocalBarrier` (the dump's `in_rendering`): on Vulkan a by-region image barrier in the local read layout, on Metal nothing, since framebuffer fetches see earlier draws at their pixel.
* **Load/store.** Per attachment of a physical pass, the load op comes from the first member attaching it: `Clear` if the access clears, `DontCare` if that member fully overwrites it or it is a transient with no earlier writer, otherwise `Load`. The store op is `Store` if the next live pass after the physical pass touching the subresource uses its contents or the texture is imported, otherwise `DontCare`.

`Execute()` records each physical pass: it writes the planned states of its members through to the trackers of the images, buffers and acceleration structures, so foreign code and the next frame start from them, then records the physical pass's barrier batch and its members, each with its own pixel-local barriers, bindings and checks. After it, it records the barriers to `HostRead` of the buffers whose last live pass it holds.

## Errors

Declaration and recording errors log their message and abort in every build, including Release, where `ASSERT` compiles out. Under test, an `RGErrorsThrow` ([RGError.h](../libraries/include/renderer/graph/RGError.h)) makes the errors on its thread throw `RGError` with the message instead. The checks:

* an invalid handle, or subresources the texture does not have;
* an access the pass kind may not declare, or a texture subresource, buffer or acceleration structure declared twice by one pass;
* two attachments in one slot, a color slot out of range, or an attachment covering more than one subresource;
* `NativeAccess()` or `PixelLocalRead` on a pass that is not a Raster pass, or a pass reading pixel-locally at a color slot it writes;
* a transient without a format or size, or an import that is missing or multisampled;
* a Raster pass without attachments, or a Compute pass without an `RHIComputePass`;
* a transient read before any pass writes it;
* an import lacking the usages its accesses need;
* attachments of one pass that differ in size;
* a sampled binding of less than every subresource, or a storage binding of more than one mip;
* a memoryless attachment the plan would load or store, which the memoryless rule excludes;
* compiling twice, or executing before compiling or twice;
* compiling a graph while another compiled graph of the same texture pool is alive: a texture pool serves one graph at a time;
* a pass that reaches a texture, buffer or acceleration structure it did not declare through its context, or records raw commands without `NativeAccess()`;
* a pass that leaves a declared image in another layout, or with accesses beyond its declaration pending;
* a binding that no pipeline its pass drew or dispatched has, in a pass that drew or dispatched;
* a graph resource bound in a pipeline a Raster or Compute pass drew or dispatched, which the pass did not declare with the access of that binding: a `Texture2D` needs `Sampled`, an input attachment `PixelLocalRead` and a `StorageImage2D` a storage access, each covering the subresources of the bound view; an `AccelerationStructure` needs `AccelerationStructureRead`; a graph buffer may not be bound at all. The check reads the resource tables when the pass ends, skipping bindless arrays.

## Debugging

### Knobs

| cvar | default | Effect |
| --- | --- | --- |
| `render_graph_view` | *(empty)* | Shows a graph texture in place of the frame (see [Viewing a Texture](#viewing-a-texture)). |
| `render_graph_cull` | `true` | Set to `false` to keep every pass, including those whose outputs no live pass uses. |
| `render_graph_merge` | `true` | Set to `false` to record every live pass as a physical pass of its own, breaking with `Disabled` where passes would merge. Frames render the same pixels either way. |
| `render_graph_memoryless` | `true` | Set to `false` to back every transient with an ordinary pooled image. Frames render the same pixels either way. |
| `render_graph_pixel_local` | `true` | Set to `false` to lower every `PixelLocalRead` to `Sampled`, breaking with `NoPixelLocalSupport` where a pass would read pixel-locally, as on a device without pixel-local reads. Frames render the same pixels either way. |
| `render_graph_full_barriers` | `false` | Before every physical pass, records one memory barrier from all commands and memory accesses to all commands and memory accesses, on top of the planned barriers. Vulkan only: Metal records nothing. A synchronization bug that disappears with it lies in a planned barrier. The planned barriers are unchanged, and the dump marks each physical pass that records the full barrier with `full_barrier: true`. |
| `render_graph_tile_budget` | `0` | The color attachment bytes per pixel a physical pass keeps in tile memory, in place of the device's budget (`0`; see [Tile budget](#what-the-compiler-decides)). |
| `render_graph_tile_budget_split` | `false` | Breaks a physical pass with `TileBudget` where the next pass would take its color attachments over the tile budget, instead of only warning. Frames render the same pixels either way. |
| `render_graph_export` | `false` | Exports the graph once the scene is ready for a screenshot (see [Exporting the Graph](#exporting-the-graph)). |
| `render_graph_profile_frames` | `0` | Makes every export aggregate the timings of that many consecutive frames of one graph structure (see [Exporting the Graph](#exporting-the-graph)). |
| `validate_sync` | `false` | Vulkan synchronization validation, which reports hazards that no planned barrier orders; needs `validation` (see [Test.md](Test.md#validation-layer)). |

### Viewing a Texture

* `render_graph_view` names a graph texture to show in place of the frame, on every renderer.
* The post chain looks it up (`RenderGraph::FindTexture`) among the textures the frame's passes created or imported before the post chain. The `GraphView` pass then samples its final contents into Screen instead of the screen pass, so culling removes every pass that only fed the scene.
* The texture must be one a pass added then can sample through a 2D binding (`RenderGraph::CanSample2D`: a single-layer import with texture usage, or a transient an earlier pass writes), of a format a float texture samples (`SamplesAsFloat` in [PostChain.cpp](../libraries/source/renderer/pass/PostChain.cpp)). Otherwise the frame shows as usual, with a warning logged once per change of the value.
* `GraphView` shows the values it samples (an sRGB texture decoded) as colors in Screen's format, which clips them, stretched over the output.
* For example, `IblBrdf` shows the BRDF map of a ready IBL, and `SceneDepth` on Deferred shows the depth `GBuffer` writes, with only `GBuffer`, `GraphView` and `Present` left live, and GBufferPacked and DepthCopy, which no live pass reads, memoryless.

### Exporting the Graph

* `Save Graph Dump` on the control panel's screenshot page (the camera icon) writes the next graph to `screenshots/<scene>_<pipeline>_<time>.json` under the [external storage path](Run.md#external-storage-paths), and states the file it is saving or saved.
* `render_graph_export` (default `false`) writes the graph to `screenshots/render_graph_export.json` once per run, as soon as the scene is ready for a screenshot. It serves headless, CI and device runs, which set it on the command line or in the config file (Android has no command line).
* `render_graph_profile_frames` (default `0`) makes every export (`Save Graph Dump`, `render_graph_export` and other `RequestGraphDump` callers) wait for that many consecutive frames whose graphs share one structure, each pass's `name`, `culled` and `physical_pass` and each physical pass's `members`, and write the last frame's dump with the timings of all of them. A frame of another structure restarts the count, with a log line. At `0` an export is one frame's dump.
* The profile adds `profile.frames`, the number of frames aggregated; `profile.cpu_ms` on each pass with a `cpu_ms` in any of them; and `profile.gpu_ms` on each physical pass with a `gpu_ms` in any of them. Each holds the `mean`, `min` and `max` over the frames that had the time and their count, `samples`. A physical pass has no `gpu_ms` on a device without pass timestamps, nor before its frame slot returns after the graph changes, so its `samples` can be lower than `frames`.
* A dump taken with a screenshot (`RequestTakeScreenshot` with `dump_graph`) is always the screenshot's frame, without a profile.

### Getting a Dump

* `RenderFramework::RequestGraphDump(name)` writes the next graph the renderer executes to `screenshots/<name>.json`, or the profile of the next ones under `render_graph_profile_frames`; `Save Graph Dump`, `render_graph_export` and the test cases use it. `RenderFramework::RequestTakeScreenshot` with `dump_graph` also writes there the graph of the frame the screenshot reads back, so the image and the dump show the same frame.
* `RenderFramework` keeps the pending requests on the render thread and serves all of them from one dump of the next graph `Renderer::Render` executes, a profiling request from each next one until it completes, so a request outlives a renderer recreation.
* From the command line, the `render_graph_dump` test case writes the graph of the first frame that is ready for a screenshot to `screenshots/render_graph.json` under the [external storage path](Run.md#external-storage-paths) and exits; any other cvars select the frame:

```bash
python3 run.py --framework glfw --test_case render_graph_dump --headless true --pipeline deferred
python3 dev/render_graph_viewer.py <external-storage-path>/screenshots/render_graph.json
```

### Dump Format

`Dump()` returns the compiled graph as JSON:

* `passes`, in declaration order: each pass's `kind`, `culled` and `cull_reason`, a live pass's `physical_pass` (an index into `physical_passes`) and `step`, `accesses`, `barriers` (images with layouts, buffers and acceleration structures without), recorded before its physical pass unless `in_rendering`, and `barriers_after`, the barriers recorded after a pass that records any (to `HostRead`, for buffers the host reads), and `cpu_ms`, the time its record function took, once executed. Each entry names its subresources unless it covers every one. A barrier from fragment work to vertex or compute work is `backward`: a tiler cannot overlap the waiting work with the fragment work it waits for. A `PixelLocalRead` request's access carries its `pixel_local_slot`, and, lowered to `Sampled`, its `lowered_reason`.
* `physical_passes`, in execution order: each one's `members` (indices into `passes`), `attachments` (slot, load and store with their reasons), `breaks`, `break_reason` and `break_resource` unless it is the last, a raster one's `color_bytes_per_pixel` and `over_tile_budget`, `full_barrier` under `render_graph_full_barriers`, and `gpu_ms` once executed with a timer that has a result.
* `resources`, textures first, then buffers and acceleration structures: each resource's `type` (`Texture`, `Buffer` or `AccelerationStructure`) and `kind` (`Transient` or `Imported`); a transient's `format`, `size_class` (with the pixel size of an `Absolute` one), `physical` image, `memoryless` and `backing` (`memoryless` or `pooled`, what the device gave); the indices of the first and last live passes that use it with its `usage`, which for a buffer or acceleration structure is the union of its accesses.
* `tile_budget`, the bytes per pixel the [tile budget](#what-the-compiler-decides) allows, when known.
* `totals`: `render_passes` (raster physical passes), `barriers` (planned, including those inside renderings and after passes), `load_bytes` and `store_bytes` (of the attachments loaded and stored, at their pixel sizes), `transient_bytes` (of the images backing transients) and `memoryless_bytes` (of those the device backs with memoryless storage).
* `opportunities`, the opportunity report: one line per physical pass, the most bytes first, with the rules the next pass breaks, the attachments it loads and stores with the bytes each moves, and its tile budget excess, e.g. `ToneMapping|Present: SlotConflict(BackBuffer), NonLocalRead(Screen); Store Screen 3.69 MB` for a physical pass ending with ToneMapping. It shows which declaration keeps passes apart and what that costs.
* The dump names size classes instead of pixel sizes, so a steady frame dumps the same passes, accesses, barriers and attachments at any resolution and on either backend. The exceptions are the byte counts of `totals` and `opportunities`, at the resolved sizes, and the `physical` assignment: transients of one format but different size classes share an image when their sizes resolve equal, e.g. `Scene` and `Output` at `render_scale` 1.

### Viewer

* `python3 dev/render_graph_viewer.py <dump.json> [-o <page.html>]` renders a dump as one self-contained static HTML page (next to the dump by default), using only the Python standard library. The page inlines [elkjs](https://github.com/kieler/elkjs) from the `thirdparty/elkjs` submodule, so it works offline from `file://`; without the submodule the viewer fails and names the fetch command, `git submodule update --init --depth 1 thirdparty/elkjs`.
* A header counts passes, physical passes, barriers and transient resources (and the memoryless ones), sums the physical passes' GPU time when the dump has timings, and lists the totals. Three tabs follow, selected by the URL hash (`#graph`, `#lifetimes`, `#grid`).
* **Graph** lays the passes out top to bottom with elk's `layered` algorithm. Each physical pass is a box (`P<index>` with its GPU time on top, every rule the next pass breaks at the bottom, an orange border over the tile budget) around its member passes, shaded by its GPU time relative to the slowest one. A pass node shows its kind, step and CPU time.
* An edge runs from the last pass that wrote a resource, or the subresources an access names, to each later pass accessing it; all resources between one pair of passes share an edge, one label line each: resource, access without stages, and between physical passes `[st]` when the writer's physical pass stores the attachment and `[ld]`, `[clr]` or `[–]` for the reader's load. A red mark on a line is a barrier before the reader. Hovering a line details the access, the attachments and the barrier with layouts and stages.
* Resource nodes are the graph's inputs and outputs: an import or transient a pass reads (or loads as an attachment) before any pass writes it is a source; the last live writers of an import feed its sink, marked when a barrier follows the write (e.g. to `HostRead`). A transient that a physical pass stores and no later live pass uses gets a red sink. Colors tell transients (blue), memoryless transients (green) and imports (purple, italic) apart, on resource nodes and label lines.
* A culled pass sits outside every physical pass, faded and dashed with its edges: it depends on the last writer among all passes, while a live pass depends only on live writers, so culled writes never feed live passes.
* `execution order` (on by default) chains consecutive physical passes with hidden edges, so the layers follow execution order; off, passes take the layers of their dependencies alone, which puts independent passes side by side.
* Drag pans and the wheel zooms; `Fit` shows the whole graph, which initially fits its width when fitting it whole would make it unreadably small. Clicking a node, physical pass or edge dims everything but what it depends on and what depends on it, and the side panel lists its details and its dump entry; clicking the background clears the selection.
* **Lifetimes** is one row per resource with its kind (an `M` badge for memoryless), format and size class, bytes per pixel, bytes for an `Absolute` size, and the physical image a transient aliases with others; a bar spans the passes from its first to its last live use, with each pass's access (`R`, `W`, `RW`).
* **Grid** is a pass × resource table: one row per pass in execution order, culled passes greyed with their reason, and one column per resource in dump order.
* A grid cell shows the pass's access (`R`, `W`, `RW`; `C` or `D` for a color or depth attachment with its physical pass's load/store) and marks a barrier before or after the pass; hovering it details accesses, subresources, attachment reasons and barriers with layouts. Shaded cells span each resource's lifetime.
* A band spans the rows of each physical pass, from its first member to its last, with a `Physical` cell naming it (`P<index>`; bold orange over the tile budget) and a `GPU ms` cell when the dump has timings; hovering it lists the members, every rule the next pass breaks, the color bytes per pixel and the attachments. The kind column names each pass's step after the first. A memoryless transient's header carries an `M` badge; hovering it names the image backing it.
* The opportunity report follows the grid.
* A dump with a profile (`profile.frames`, and `profile.cpu_ms` per pass and `profile.gpu_ms` per physical pass, each with `mean`, `min`, `max` and `samples`) shows each time as its mean with `[min–max]`, and the frames that sampled it out of the profiled ones; GPU shading and the header's sum use the means.

### Pass Timing

* `Execute(command_context, timers)` times raster physical passes through an `RGPassTimers`, which holds one timed `RHIPass` per physical pass name, the member names joined by `+` (e.g. `BasePass+SkyBox`); without one they are untimed. `Renderer::ExecuteGraph` passes the renderer's timers.
* A Metal timer samples the render pass descriptor, so only whole physical passes are timed; members get debug labels of their own inside the physical pass's label.
* A time arrives `max_frames_in_flight` frames after its physical pass records (see [RHIPass.h](../libraries/include/rhi/RHIPass.h)) while a graph lives one frame, so the owner of the graphs keeps the timers across frames. A physical pass's `gpu_ms` is the time its timer measured when it last ran in the current frame slot, at least `max_frames_in_flight` frames before the dumped frame.
* Physical passes sharing a name share a timer and report its last run.
* Compute passes report the time of their `RHIComputePass`, which measures only when created timed.
* Copy and External passes are untimed: a Metal timer covers one encoder, while a Copy pass opens one per copy, and foreign code times its own work (NRD, MetalFX).
* Timing needs `RHIContext::SupportsPassTimestamps()`.

### Memory Traffic on Apple GPUs

* `python3 dev/apple_gpu_traffic.py [--runs 3] [--seconds 6] [--window 3] -- <app arguments>` measures the GPU memory traffic of each physical pass of the macOS app, e.g. `-- --pipeline deferred --render_graph_merge false` against `-- --pipeline deferred`. Each run records the app headless with `xctrace` for `--seconds`, keeps the last `--window` seconds (after startup and scene loading) and quits the app; the script prints each run's totals and the median over the runs of the bytes read and written per frame and the GPU time per frame of each encoder label (a physical pass's members joined by `+`) and their total.
* The recording uses [dev/instruments/SparklePerformanceLimiters.tracetemplate](../dev/instruments/SparklePerformanceLimiters.tracetemplate): Metal System Trace with the Metal Application instrument's Counter Set at `Performance Limiters`, whose 88 counters include GPU read and write bandwidth, last-level-cache and imageblock L1 bandwidth and the shader limiters, and its Performance State at `Maximum`, which pins the GPU clocks so GPU times repeat within about 1%. With the Counter Set at `None` (the default) Instruments samples no bandwidth counter, and `xctrace record --instrument` cannot set it. To recreate the template: Instruments, Metal System Trace, select Metal Application, set Counter Set and Performance State, then File > Save As Template.
* A counter sample averages the bandwidth over tens of microseconds, about the length of a pass, so its bytes go to the GPU work that ran in it in proportion to the time each ran (vertex and fragment work of an encoder merged). The GPU samples its counter groups in turn, so the bandwidth counters cover only part of each pass's GPU time (50% to 98% per run); a pass's bytes are scaled up to its whole GPU time, and runs still differ by about 20%, which is why the script reports a median.
* The counters are device-wide, so the script refuses to record while another sparkle instance runs and reports how much of the app's GPU time other processes' GPU work overlapped.
* Apple GPUs compress render targets losslessly, so stored bytes fall below the dump's `store_bytes` (which counts the uncompressed images); compare configurations against each other.
* Exported tables and traces stay in `build_system/macos/output/gpu_traffic/run<N>/` (`--output`).

## Renderers

Each renderer builds one graph per frame from the `RenderFramework`'s texture pool, which outlives renderer recreation, so a pipeline switch reuses the images both pipelines' graphs need.

Only a pipeline switch recreates the renderer (with the scene's render proxies, after a device-idle wait). A resolution change (`width`, `height`, `render_scale`, or a window resize, which sets `width` and `height`) resizes it in place: transients resolve their size classes against each frame's `RenderResolution`, and the pool releases images of the old sizes once no graph uses them. `Renderer::Tick` calls the renderer's `OnResize` hook for its persistent, imported resources before the frame updates: the GPU renderer recreates the accumulator, reallocates the denoiser inputs and resizes each denoiser in place (`Denoiser::Resize`), and the CPU renderer reallocates its host buffers. The camera proxy recomputes its projection and restarts accumulation when the scene resolution changes. Images a frame in flight still uses are released through the RHI's deferred deletion. The goldens in [tests/render_graph/golden/](../tests/render_graph/golden/) list the passes, accesses, barriers, attachments and resources of a typical frame of each renderer (see [Tests](#tests)); the [viewer](#viewer) renders a dump of any frame.

Each attachment role has one color slot in every pass that attaches it ([ColorSlot.h](../libraries/include/renderer/pass/ColorSlot.h)): Screen and BackBuffer 0, SceneColor 1, GBufferPacked 2, DepthCopy 3. Other attachments, such as the GPU accumulator and the IBL cook clears, use slot 0.

A pixel shader writes a role through its output struct in [color_slot.h.slang](../shaders/include/color_slot.h.slang), whose member carries an explicit `vk::location`: Slang places a returned `SV_Target<slot>` at output 0. A pass that attaches only a higher slot leaves the lower ones empty.

Every frame ends with the post chain (`PostChain`, [PostChain.h](../libraries/include/renderer/pass/PostChain.h)), added after the scene passes, which leave the scene in `scene`:

* **Screen pass.** It draws `scene` into Screen, a transient at output resolution. The renderer chooses it and Screen's format once (`Renderer::InitPostChain`):
  * `ToneMapping` into `B8G8R8A8Srgb` for the renderers that tone map on the GPU;
  * `Upsample` into `RGBAFloat16` for the CPU renderer, which draws only when `render_scale` < 1; otherwise `scene` is the screen.
  * `GraphView` replaces the screen pass while `render_graph_view` shows a texture.
* **Readback.** A Copy pass copies the screen into a staging buffer the host reads when a screenshot is pending.
  * It runs before `Ui` for a screenshot without UI and after it for one with UI.
  * A screenshot with UI gets the screen without UI when `Ui` does not draw.
  * The buffer is created with the pass (its size comes from `RenderGraph::GetFormat` and `GetSize`), imported, and saved once the frame completes.
* **Ui.** When UI is shown and the app is not headless, it draws ImGui into Screen, through `NativeAccess()`.
  * It joins the physical pass of the screen pass, in the same step, since both attach only Screen at slot 0; on Forward, that physical pass also holds SceneColor at slot 1 and SceneDepth, and on Deferred also GBufferPacked and DepthCopy at slots 2 and 3. Without a screen pass, or after the readback of a screenshot without UI, it starts a physical pass.
  * ImGui draws against the attachment signature of that physical pass, writing slot 0 only. Its Vulkan backend holds one pipeline. `VulkanUiHandler` recompiles it when the signature changes, waiting for the device to idle first, because ImGui destroys the previous pipeline at once. Its Metal backend caches a pipeline per set of attachment formats.
  * The main thread builds ImGui frames while the render thread draws earlier ones, so ImGui's textures stay on the main thread: it acknowledges their requests (e.g. new font glyphs) into imgui_club's `ImTextureQueue` (`RHIUiHandler::BeginImGuiFrame`, `QueueTextureRequests`), and the handler serves them before it draws the copied draw data. The queue destroys a texture only after the render thread adopted a frame that no longer draws it and the frames in flight retired, and the handler lives until `RHIContext::Cleanup`, since the queue cannot hand its textures back while the main thread runs ImGui.
  * ImGui leaves its own viewport and scissor set, which a later member of the physical pass would inherit. None follows `Ui`: `Present` attaches BackBuffer at Screen's slot and samples Screen.
* **Present.** It draws the screen into BackBuffer, the image `RHIContext::GetBackBuffer()` returns.
  * On a windowed Vulkan device, BackBuffer is the acquired swap chain image.
  * Otherwise it is one image, whose Metal texture is the frame's drawable when windowed.
  * It applies the window's pre-rotation.

The screen passes and `Present` are `ScreenQuadPass`es built from their output format and the filter they sample their input with (`ScreenQuadPass::InputFilter`):

* They always sample edge-clamped.
* `ToneMapping`, `Upsample` and `GraphView` sample bilinearly when the input's size differs from the output's and the device filters the input's format linearly, otherwise nearest.
* `ToneMapping` requests a `PixelLocalRead` of its input at SceneColor's slot, drawing with its `PIXEL_LOCAL` shader variant where the read stays pixel-local and sampling as above where it is lowered.
* Linear filtering of 32-bit float and depth formats is optional on both backends (`RHIContext::SupportsLinearFiltering`).
* `Present` samples nearest when BackBuffer's size, along the rotated axes, is an integer multiple of Screen's in both axes (1:1 included), otherwise as the other screen passes.

The renderers:

* **CPU.**
  * The path tracer tone maps on the host into a host buffer, imported as HostSceneColor.
  * The `Upload` Copy pass copies it into the SceneColor transient at scene resolution.
  * SceneColor is the screen at `render_scale` 1; `Upsample` draws it into the screen when `render_scale` < 1.
* **GPU.**
  * The accumulator, the TLAS, the denoiser inputs (once a denoiser has needed them) and the denoiser outputs are imports.
  * `ClearAccumulator` (a Raster pass with no draws) clears the accumulator when the camera moved or the scene changed.
  * `BuildTLAS` (Copy) records the build or refit `GPURenderer::Update` staged on the TLAS.
  * `PathTrace` (Compute) traces while accumulating, on the renderer's timed `RHIComputePass`, which dynamic spp reads back.
  * `PathTrace` declares the six denoiser inputs as storage writes once they are allocated, because the tracer binds them on every dispatch.
  * A denoiser adds one External pass (see [Denoiser.md](Denoiser.md)).
  * Tone mapping displays the provider's output when it encodes the frame.
  * It displays the accumulator when a frame traces without denoising or the provider cannot encode it.
  * Otherwise it displays whatever it displayed last: a converged or paused frame keeps the last denoised output, imported as DenoiserOutput.
  * Tone mapping samples what it displays: its pixel-local read is lowered (`NonRasterPass` or `ExternalPass` after the pass that wrote it, `NoWriter` for a kept output).
* **Forward.**
  * `DirectionalShadow` renders the ShadowMap transient (`shadow_map_resolution` squared) when the scene has a directional light.
  * `BasePass` writes the SceneColor and SceneDepth transients.
  * `SkyBox` draws the sky map over them while depth-testing, in the same physical pass and step as `BasePass`, so SceneDepth, which no later pass reads, is memoryless.
  * `ToneMapping` reads SceneColor pixel-locally in a step of its own of that physical pass, so SceneColor is memoryless too. Its read is lowered to `Sampled`, after the physical pass ends, when `render_scale` < 1 (`TargetSizeMismatch`) and without pixel-local reads (`NoPixelLocalSupport`).
  * The IBL maps and the sky map are imports; an IBL map is imported only once ready.
  * `BasePass` samples the shadow map and IBL maps as `LightingInputs`, where a missing one (no directional light, a map still cooking) samples a placeholder.
  * `SkyBox` samples the map the output mode selects, with the sampler it is read with (`GetSkyBoxMap` in [RasterRenderer.cpp](../libraries/source/renderer/renderer/RasterRenderer.cpp)).
  * Tone mapping upsamples when `render_scale` < 1, so the graph has the same passes at any scale.
* **Deferred.**
  * Forward and Deferred derive from `RasterRenderer`, which adds everything but the passes that draw the scene (`AddScenePasses`), so Deferred has the same shadow, IBL maps, sky box and post chain.
  * `GBuffer` writes the GBufferPacked, DepthCopy and SceneDepth transients. DepthCopy is an `R32Float` color attachment holding the device depth (`SV_Position.z`) the depth attachment stores, cleared to the far plane like it, because Metal cannot read a depth attachment pixel-locally.
  * `Lighting` reads GBufferPacked and DepthCopy pixel-locally, reconstructs world positions from the depth copy and writes SceneColor, without a depth attachment. `SkyBox` depth-tests against SceneDepth and `ToneMapping` reads SceneColor pixel-locally, so `GBuffer`, `Lighting`, `SkyBox` and `ToneMapping` form one physical pass, which `Ui` joins, with GBufferPacked, DepthCopy, SceneDepth and SceneColor memoryless: 32 bytes of color per pixel (16, 4, 8 and 4 for Screen) plus the depth.
  * Lowered, `Lighting` loads both textures at its pixel, so it shades the same values: without pixel-local reads the physical pass ends after `GBuffer` and after `SkyBox` (`NoPixelLocalSupport`), and at `render_scale` < 1 after `SkyBox` (`TargetSizeMismatch`).
* **IBL cook.**
  * While an IBL map cooks on the GPU, `ImageBasedLighting::AddCookPasses` starts the Forward or Deferred graph with one cook step per map still cooking.
  * The first step clears each subresource of the map being cooked in its own Raster pass (`ClearIblBrdfCook`, `ClearIblDiffuseCook`, `ClearIblSpecularCook`).
  * `CookIblBrdf`, `CookIblDiffuse` (all six faces as a 2D array) and `CookIblSpecular` (the mip being cooked) are Compute passes that read and write the cooking map as storage.
  * The diffuse and specular cooks also sample the sky map.
  * The cooking maps are other images than the maps the scene passes sample.
  * A finished map leaves its cook with every subresource in one tracked state.
  * `IblCookAccelerator` drives a pass to completion in frames of its own, each a graph holding one cook step.

## Tests

* `render_graph_compile` builds synthetic graphs, compares their dump summaries against expected plans (culling, image sharing within and across frames and within a physical pass, physical passes and every break reason with and without merging, every rule a boundary breaks and the opportunity report, the tile budget warning and its `TileBudget` break under an injected budget, per-subresource barriers and mip runs, load/store with reasons, memoryless transients and the images backing them, pixel-local reads and every reason they are lowered for, buffer and acceleration-structure barriers, full barriers, bindings and placeholders), executes them and reads the results back ([RenderGraphCompileTest.cpp](../tests/render_graph/RenderGraphCompileTest.cpp)). A depth test against a stored depth clear, which fails everywhere, shows that the depth keeps its contents in a frame without merging after a frame where it was memoryless. A merged quad that samples a texture written before its physical pass reads back the texture, the slot it does not write keeps the earlier member's clear, and a depth test the quad's pipeline would fail is ignored. Apple GPUs keep an unwritten slot's contents even without the write mask, so there only Vulkan drivers that write undefined values catch a missing mask. Tone mapping reads a clear of an earlier member pixel-locally, with the barrier its read waits on inside the rendering, and reads back the texel it reads sampled with pixel-local reads off. `render_graph_sync_validation` runs it under Vulkan synchronization validation (see [Test.md](Test.md#validation-layer)).
* `render_graph_errors` makes one mistake per error with errors thrown, and expects each error's message.
* `render_graph_pass_timing` executes a graph of a Compute pass and two Raster passes that merge into one physical pass every frame with pass timers kept across frames, and expects both physical passes to dump a `gpu_ms` once their frame slot returns, or none on a device without pass timestamps.
* `render_graph_merge_parity` screenshots a Forward frame (`render_graph_merge_parity_deferred` a Deferred one) and dumps the graph of the same frame with `render_graph_merge`, `render_graph_memoryless` and `render_graph_pixel_local` on, then with memoryless off, then with pixel-local reads off, then with merging off, switching each at runtime: the screenshots must be bit-identical, only the first three graphs may merge passes, only the first and third may have memoryless transients, and only the first two read pixel-locally, on a device with pixel-local reads, where the first has memoryless transients. Before the first capture, every configuration renders a few frames once, so no capture is among the first frames a pipeline draws, which a driver may render slightly differently. A configuration's screenshot counts once a later capture of it is bit-identical to an earlier one, within four captures, because a driver may also render an occasional later frame differently (the macOS runners' paravirtual GPU does, as sparse speckles on mip-mapped textures after a configuration switch); the captures of a configuration that needed more than two are kept in `screenshots/captures/`. The parity cases set `sampler_anisotropy` off, because anisotropic sampling is not deterministic from frame to frame on every GPU. `render_graph_merge_parity_sync_validation` and `render_graph_merge_parity_deferred_sync_validation` run them under Vulkan synchronization validation.
* `render_graph_ui` runs windowed, where the UI draws: it saves a screenshot with the UI to `screenshots/render_graph_ui.png` and the graph of a frame to `screenshots/render_graph_ui.json`, then switches to the CPU pipeline, whose screen has another format, and saves both again as `render_graph_ui_cpu`. Each graph must have a live `Ui` in the physical pass of the Raster pass before it, or, after a pass of another kind (the CPU frame's `Upload` copy), in a physical pass of its own that the pass before it ends with `NonRasterPass`. It is a development case with no registry entry: headless runs never create ImGui, and a windowed macos run stalls while its window is occluded. With `--validation true` (and `--validate_sync true`) it checks the Vulkan UI under validation, with `MTL_DEBUG_LAYER=1` the Metal one.
* `render_graph_export` turns `render_graph_export` on and checks the exported dump: without `render_graph_profile_frames` it has no profile; with it (`render_graph_export_profile`, 30 Deferred frames) the profile has that many frames, every pass the dumped frame timed has as many `samples`, and each timing's `min` and `max` bound its `mean` and the dumped frame's time.
* `pipeline_switch_pool` switches pipelines at runtime and requires each new renderer to reuse an image the previous one left in the texture pool.
* [tests/build_system/test_render_graph_viewer.py](../tests/build_system/test_render_graph_viewer.py) unit-tests the viewer and the golden projection.

### Golden Graph Shapes

* Each renderer has a golden graph shape in [tests/render_graph/golden/](../tests/render_graph/golden/) (`<pipeline>.txt`), checked by a `<pipeline>_graph_shape` registry case. `deferred_graph_view_shape` checks a deferred frame viewing SceneDepth against `deferred_view.txt`, `deferred_graph_view_fallback` one naming the integer GBufferPacked against `deferred.txt`, `forward_graph_shape_no_merge` a forward frame with `render_graph_merge` off against `forward_no_merge.txt`, and `forward_graph_shape_halfres` one at `render_scale` 0.5, whose tone mapping samples SceneColor, against `forward_halfres.txt`.
* The shape cases run the `render_graph_dump` test case, which waits until the scene is ready for a screenshot and asks the renderer (`RenderFramework::RequestGraphDump`) to write the next graph it executes to `screenshots/render_graph.json`, which the device runners pull like screenshots.
* A converged GPU frame traces nothing, so `gpu_graph_shape` runs `render_graph_dump_accumulating`, which dumps frames once the scene is loaded until one traces onto samples the accumulator already holds with no TLAS build (a live `PathTrace`, no `ClearAccumulator` or `BuildTLAS`), so the frame does not depend on when loading finished. Its golden is a frame without a denoiser, clear or screenshot, and needs ray query support (lavapipe has it).
* [tests/render_graph/graph_shape_test.py](../tests/render_graph/graph_shape_test.py) projects the dump to one line per pass, access, barrier, physical pass (after its last member, with its members and every rule the next pass breaks), attachment and resource (with `memoryless`), leaving out times, byte counts, the tile budget and the device's `backing`, and prints a unified diff against the golden.
* It also renders the dump through the viewer to `screenshots/captures/render_graph_<case>.html` (its `--page`) and fails the case if that raises. CI uploads the pages with the test screenshots (the `test-screenshots-<framework>-<os>` artifact).
* The goldens are frames of the default TestScene, so the shape cases ignore the suite's `--scene`. The forward and deferred goldens are frames with a directional light, a sky map and ready IBL maps.
* Tests run headless, so the goldens show no `Ui` pass, and the back buffer's first barrier has no `Present` access to wait for.
* The goldens hold the pixel-local plan, which every device of the golden cells (macos-macos, ubuntu-glfw) supports. The sampled lowering is screenshot-tested instead: `forward_render_static_sampled` and `deferred_render_static_sampled` turn `render_graph_pixel_local` off on ubuntu-glfw, and the android emulator and the iOS simulator, which have no pixel-local reads, take it in every case.

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
