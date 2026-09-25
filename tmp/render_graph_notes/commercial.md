# Production Render Graphs: A Survey for a Lean Design

Scope: how mature commercial and production render graphs work, taken from primary sources wherever they exist: official docs, public source code (Unity SRP, Godot, O3DE, AMD RPS), and GDC/SIGGRAPH/REAC slides. Unreal's source is under an EULA, so its internals come from official docs, the public API reference, cvar descriptions extracted from source (the indxzero cvar wiki), and well-known public source analyses. Each claim has a URL.

---

## 1. Unreal Engine RDG (Render Dependency Graph)

### 1.1 Core model

- RDG is "an immediate-mode API which records render commands into a graph data structure to be compiled and executed". It automates error-prone operations and walks the graph to optimise memory and to parallelise passes on CPU and GPU. [UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- Setup and execute run on separate timelines. The graph is built during **setup**. All RHI commands are deferred into pass lambdas that run on the **execute** timeline. Lambdas "should be free from side effects and simply record commands into the command list", because pass execution may run in parallel. [UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- Resources are virtual until compile. `CreateTexture`/`CreateBuffer` return RDG handles (descriptors only). Calling `Texture->GetRHI()` outside a pass lambda asserts. [UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)

### 1.2 API sketch

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
  SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)          // SRV read -> dependency edge
  SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputUAV)       // UAV write
  RDG_TEXTURE_ACCESS(CopySrc, ERHIAccess::CopySrc)               // non-shader access (copies etc.)
  RENDER_TARGET_BINDING_SLOTS()                                  // raster attachments + load actions
END_SHADER_PARAMETER_STRUCT()

FRDGBuilder GraphBuilder(RHICmdList);
FRDGTextureRef Tex = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(...), TEXT("MyTexture"));
auto* P = GraphBuilder.AllocParameters<FParameters>();
P->OutputUAV = GraphBuilder.CreateUAV(Tex);
GraphBuilder.AddPass(RDG_EVENT_NAME("MyPass"), P, ERDGPassFlags::Compute,
    [P](FRHIComputeCommandList& RHICmdList) { /* record only */ });
GraphBuilder.QueueTextureExtraction(Tex, &PooledOut);   // survive past the graph
GraphBuilder.Execute();
```
Snippets are adapted from [UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine). For raster passes, `RenderTargets[i] = FRenderTargetBinding(Tex, ERenderTargetLoadAction::EClear)` and `RenderTargets.DepthStencil = FDepthStencilBinding(...)` fill the binding slots.

**Declaring dependencies.** The pass parameter struct is the dependency declaration. Its macros produce reflection metadata, and RDG walks that metadata to get every read and write (SRV, UAV, RT, `RDG_*_ACCESS`). "Resource barriers and lifetimes are derived from RDG parameters in the pass parameter struct" ([FRDGBuilder API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder), [timlly RDG analysis](https://www.cnblogs.com/timlly/p/15217090.html)). The same struct also binds shader parameters, so one declaration does both jobs. The cost is that an unused shader parameter still creates a dependency. Epic's answer is `ClearUnusedGraphResources(Shader, Params)`, which nulls out unused bindings so they don't extend lifetimes ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)).

**Pass flags** ([ERDGPassFlags](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/ERDGPassFlags)): `Raster`, `Compute`, `AsyncCompute`, `Copy`, `Readback`, `NeverCull` ("Pass (and its producers) will never be culled"), `SkipRenderPass` ("Render pass begin / end is skipped and left to the user"), `NeverMerge`, `NeverParallel` ("Pass will never run off the render thread").

**Texture flags** ([ERDGTextureFlags](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/ERDGTextureFlags)): `MultiFrame`, `SkipTracking` ("ignored by RDG tracking and will never be transitioned"), `ForceImmediateFirstBarrier` ("first barrier without splitting"), `MaintainCompression`.

**Other builder API** ([FRDGBuilder API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder)): `AddSetupTask` / `AddCommandListSetupTask` ("Launches a task that is synced prior to graph execution"), `FlushSetupQueue`, `AddDispatchPass` (the pass itself fans out command lists), `AddPassDependency` (an explicit edge), `SetPassWorkload`, `UseExternalAccessMode` / `UseInternalAccessMode`, `SetTextureAccessFinal`, `SkipInitialAsyncComputeFence`, `AddPostExecuteCallback`, and `AllocObject/AllocPOD/AllocArray` for memory that lives as long as the graph.

### 1.3 Compile: culling, barriers, merging

- **Culling.** Passes with no consumers are culled. `NeverCull` protects side-effect passes. Roots are passes that write external or extracted resources or carry `NeverCull`, and a traversal from the roots marks live passes. Toggle with `r.RDG.CullPasses`. ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), [timlly](https://www.cnblogs.com/timlly/p/15217090.html))
- **Barriers.** RDG tracks per-subresource state and emits **split barriers** ("to hide latency and improve overlap on the GPU"). Each pass has prologue and epilogue barrier batches (`FRDGBarrierBatchBegin/End`, `GetEpilogueBarriersToBeginForGraphics/ForAsyncCompute`). A transition can begin on one pipe and end on the other. ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), [timlly](https://www.cnblogs.com/timlly/p/15217090.html))
- **Render pass merging.** `r.RDG.MergeRenderPasses` (default 1): "The graph will merge identical, contiguous render passes into a single render pass" ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.mergerenderpasses/)). Mechanically, consecutive raster passes with identical render-target bindings (`CanMergeBefore`), the same GPU mask, and no `SkipRenderPass`/`NeverMerge` share one RHI render pass. The first pass sets `bSkipRenderPassEnd`, middle passes set both skip flags, and the last pass sets `bSkipRenderPassBegin` ([timlly](https://www.cnblogs.com/timlly/p/15217090.html)). **This merges identical render passes only. It does not form subpasses.** RDG never builds a subpass chain from passes with different attachments.

### 1.4 Transient memory and aliasing

- `r.RDG.TransientAllocator`: 0 = off (falls back to the pooled render-target pool), 1 = all transient resources (default), 2 = only `FastVRAM` resources. RDG uses `FRHITransientResourceAllocator` ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.transientallocator/)). "Resources with disjoint lifetimes may overlap in memory". On platforms with a transient allocator this gives a "significant reduction in the GPU memory watermark over the default resource pool approach" ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)). Aliasing works on heaps and depends on platform support. Imported and external resources are never aliased.
- Debug knobs: `r.RDG.Debug.ExtendResourceLifetimes` (turns aliasing off), `r.RDG.Debug.DisableTransientResources`, `r.RDG.Debug.ClobberResources` (fills new resources with a known value to catch reads of uninitialised data). ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine))

### 1.5 External and extracted resources

`RegisterExternalTexture(PooledRT)` imports a persistent resource. `QueueTextureExtraction(Tex, &OutPooled)` exports a graph texture after `Execute` (history buffers, for example). `ConvertToExternalTexture` allocates immediately and gives up aliasing. `UseExternalAccessMode` hands a resource to non-RDG code within the frame. `SetTextureAccessFinal` pins the resource's end-of-graph state. ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), [FRDGBuilder API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder))

### 1.6 Async compute

- Async compute is opt-in per pass through `ERDGPassFlags::AsyncCompute` with an `FRHIComputeCommandList`. "RDG traverses the graph to find the last producer on the graphics pipeline and inserts a fence", and joins "when the work is first consumed on the graphics pipeline" ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)). Internally, `CrossPipelineProducer/Consumer`, `bAsyncComputeBegin/End` and `GraphicsForkPass/GraphicsJoinPass` extend resource lifetimes across the async interval so aliasing stays safe ([timlly](https://www.cnblogs.com/timlly/p/15217090.html)).
- `r.RDG.AsyncCompute`: 0 = off, 1 = tagged passes (default), 2 = "all compute passes implemented to use the compute command list" ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.asynccompute/)). Scheduling is **not automatic** in the usual sense. The engineer decides which passes go async, and RDG only computes the fences.

### 1.7 Parallel setup and execute

- `r.RDG.ParallelExecute` (default 1): "Whether to enable parallel execution of passes when supported". `IsParallelExecuteEnabled()` also requires no command-list bypass, no immediate mode, no debug mode, and **a non-mobile platform** ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.parallelexecute/)).
- `r.RDG.ParallelExecute.PassMin` (default 1): "The minimum span of contiguous passes eligible for parallel execution for the span to be offloaded to a task". `FRDGBuilder::SetupParallelExecute` checks `ParallelPassCandidateCount >= PassMin`. A `PassMax` cvar caps span size ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.parallelexecute.passmin/)). Contiguous runs of parallel-safe passes become tasks, and each task records into its own `FRHICommandList`. The lists are submitted in original order ("the order of submission of commands to the GPU is unchanged from the order the commands would have been submitted in a single-threaded renderer", [Parallel Rendering Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/parallel-rendering-overview-for-unreal-engine)).
- A pass cannot run in parallel if it uses `FRHICommandListImmediate` or carries `NeverParallel` ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), [ERDGPassFlags](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/ERDGPassFlags)).
- `r.RDG.ParallelSetup` (default 1): "RDG will setup passes in parallel when prompted by calls to FRDGBuilder::FlushSetupQueue". Its conditions include `!IsMobilePlatform(...) && !IsOpenGLPlatform(...)` ([cvar](https://indxzero.github.io/ue544cvarwiki/articles/r.rdg.parallelsetup/)). Setup work such as shader-parameter processing and `AddSetupTask` jobs runs on worker tasks.
- **Mobile gets neither parallel setup nor parallel execute.** This matters for Android and iOS targets.

### 1.8 Mobile: subpasses outside RDG's automation

- The RHI has `ESubpassHint { None, DepthReadSubpass, DeferredShadingSubpass ("Mobile deferred shading subpass"), CustomResolveSubpass }` ([ESubpassHint](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/ESubpassHint)). It is set in `FRenderTargetBindingSlots::SubpassHint`.
- Mobile deferred keeps the GBuffer "in tile memory inside the GPU, meaning that the GBuffer is never stored in system memory". iOS uses framebuffer fetch, Android Vulkan uses subpasses, and GLES uses PLS or framebuffer-fetch extensions. There is no MSAA, and Mali is limited to 16 bytes per pixel and 4 input attachments. ([UE mobile deferred](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-the-mobile-deferred-shading-mode-in-unreal-engine))
- Mechanically, `FMobileSceneRenderer::RenderForwardSinglePass/RenderDeferredSinglePass` records the whole base pass, decals, deferred lighting and translucency sequence inside **one** render pass and calls `RHICmdList.NextSubpass()` between phases. The GBuffer targets are created `TexCreate_Memoryless` ([timlly mobile analysis](https://www.cnblogs.com/timlly/p/15511402.html)). In UE5 these functions take an `FRDGBuilder&` and a `FMobileRenderPassParameters*`, so RDG sees the chain as one raster pass. The subpass structure is hand-authored with a hint, not inferred from the graph.

### 1.9 Debugging and visualisation

- **Immediate mode**: `r.RDG.ImmediateMode` / `-rdgimmediate` runs each pass inside `AddPass`, giving one call stack. Combine with `r.RHICmdBypass -onethread`. ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine))
- **Validation layer**: on in Debug/Development, compiled out in Test/Shipping. It raises fatal checks that name the offending resource or pass.
- `r.RDG.Debug` warns about inefficiencies. `r.RDG.TransitionLog` / `-rdgtransitionlog` logs transitions, and `r.RDG.Debug.ResourceFilter` / `PassFilter` narrow the output. RHI validation can log a single resource with `-rhivalidationlog=Name`.
- **RDG Insights** (Unreal Insights plugin, `-trace=rdg,defaults`) shows "Resource lifetimes, pass associations, and resource pool allocation overlap; Asynchronous compute fences and overlap; Graph culling and render pass merging; Parallel execution pass ranges; Transient Memory Layouts". ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine))
- Every compile decision can be turned off with a cvar (`CullPasses`, `MergeRenderPasses`, `ParallelExecute`, `TransientAllocator`, `AsyncCompute`). That makes A/B bisection of graph bugs cheap, and it is worth copying.

### 1.10 Pain points and tech debt

- **Lambda lifetime traps.** Captured stack variables must outlive execution, so you need `AllocObject`/`AllocPOD`. Referencing RHI resources during setup asserts. ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine))
- **Macro-heavy parameter structs.** Dependencies and shader bindings are fused together. Unused parameters create false dependencies (hence `ClearUnusedGraphResources`). Signature mismatches produce large template errors ([GroundZer0](https://medium.com/@GroundZer0/rdg-in-practice-from-shader-parameters-to-a-working-pass-3f61d5108e7f)).
- **The RDG/legacy boundary.** Passes outside RDG need manual barriers and a register/extract round trip ([stoleckipawel production survey](https://stoleckipawel.dev/posts/frame-graph-production/)). Hence the escape hatches `UseExternalAccessMode`, `SkipTracking`, `SkipRenderPass`, `NeverParallel` and `Readback`. Each one exists to interoperate with code the graph cannot see.
- **Stack traces are useless during execute**, because every transition happens at the same place. Immediate mode and transition logs are the workaround ([UE RDG doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)).
- **Mobile is a second-class citizen**: no parallel setup or execute, and subpasses are manual.
- **Async placement is manual.** Aliasing covers transient resources only.

---

## 2. Unity SRP RenderGraph (URP 17 / Unity 6, HDRP)

### 2.1 Status and history

- It first appeared behind a define in 2023.3.0a6 and was on by default from 2023.3.0a18. The stated goal: "an average of 1ms improvement in GPU performance per frame, significantly reducing bandwidth waste", mainly by applying "the NativeRenderPass API that optimizes GPU bandwidth on tile-based (mobile) GPUs". ([Unity forum intro](https://discussions.unity.com/t/introduction-of-render-graph-in-the-universal-render-pipeline-urp/930355))
- Compatibility Mode (the non-graph path) was moved behind `URP_COMPATIBILITY_MODE` in 6.3 ([Upgrade to 6.3](https://docs.unity3d.com/6000.4/Documentation/Manual/UpgradeGuideUnity63.html)). In 6.4 it is "fully removed for custom render passes" ([Upgrade to 6.4](https://docs.unity3d.com/6000.4/Documentation/Manual/UpgradeGuideUnity64.html)). Keeping two paths alive cost Unity roughly three release cycles, which is a direct argument against dual paths.
- In 6000.5, `RenderGraph.nativeRenderPassesEnabled` is marked `[Obsolete("RenderGraph always enables native render pass support.")]`. Its doc comment says that turning native render passes on invalidated "Any AddRenderPass overloads. The more specific AddRasterRenderPass/AddComputePass/AddUnsafePass functions should be used", because the old API "can't express detailed frame information needed to emit native render passes" ([RenderGraph.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraph.cs)). **This is why the pass types were split.** The generic pass had a free-form command buffer that could call `SetRenderTarget` at any time, so the compiler could not know a pass's attachments ahead of time and could not merge passes into native render passes.

### 2.2 API sketch

```csharp
using (var builder = renderGraph.AddRasterRenderPass<PassData>("Copy To Debug", out var passData))
{
    passData.src = frameData.activeColorTexture;
    TextureHandle dst = renderGraph.CreateTexture(desc);
    builder.UseTexture(passData.src);                 // sampled read (AccessFlags.Read default)
    builder.SetRenderAttachment(dst, 0);              // color attachment slot 0
    // builder.SetRenderAttachmentDepth(depth, AccessFlags.Write);
    // builder.SetInputAttachment(gbuf0, 0, AccessFlags.Read);  // framebuffer fetch / subpass input
    builder.AllowPassCulling(false);                  // docs: don't do this in production
    builder.SetRenderFunc(static (PassData d, RasterGraphContext ctx) => Execute(d, ctx));
}
```
([Write a render pass](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-write-render-pass.html). Static lambdas are required to avoid allocations.)

Builder surface ([RenderGraphBuilders.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphBuilders.cs)): `UseTexture/UseBuffer(handle, AccessFlags)`, `UseGlobalTexture`, `UseAllGlobalTextures`, `SetGlobalTextureAfterPass`, `SetRenderAttachment`, `SetRenderAttachmentDepth`, `SetInputAttachment`, `SetRandomAccessAttachment`, `SetShadingRateImageAttachment`, `CreateTransientTexture/Buffer`, `EnableAsyncCompute`, `AllowPassCulling`, `AllowGlobalStateModification`, `EnableFoveatedRasterization`, `GenerateDebugData`.

**The three pass types:**
- **Raster**: gets a `RasterCommandBuffer` with no `SetRenderTarget`, so its attachments are fully declared. Only raster passes can be merged.
- **Compute**: gets a `ComputeCommandBuffer`. It can go async (`EnableAsyncCompute`), and it always ends a native render pass.
- **Unsafe**: gets an `UnsafeCommandBuffer` with `SetRenderTarget`. "Rendering might be slower because the render graph system can't optimize the render pass." You cannot call `SetRenderAttachment` here. ([Unsafe pass](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-unsafe-pass.html))

**Global state** is a Unity-specific wart. Shaders read global textures, so a pass that sets globals must say so (`AllowGlobalStateModification`), and that setting **disables culling** for the pass (`AllowPassCulling(true)` becomes a no-op) ([RenderGraphBuilders.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphBuilders.cs)).

### 2.3 The NativePassCompiler (read from source)

`NativePassCompiler.Compile()` runs these stages in order ([NativePassCompiler.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/Compiler/NativePassCompiler.cs)):
```
ValidatePasses → SetupContextData → BuildGraph → CullUnusedRenderGraphPasses → TryMergeNativePasses
→ HandleExtendedFeatureFlags → FindResourceUsageRangeAndSynchronization → DetectMemoryLessResources
→ PrepareNativeRenderPasses → (PropagateTextureUVOrigin) → CompactNonCulledPassesForRasterPasses
```
- **Culling** runs twice: it removes passes with no side effects, then passes that write only unused resources.
- **Merging** is a greedy linear scan in submission order (`TryMergeNativePasses`), with no reordering. `CanMerge` returns a **`PassBreakAudit` with a reason** ([PassesData.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/Compiler/PassesData.cs)). The full `PassBreakReason` list is worth copying as a design checklist:
  - `TargetSizeMismatch`: width, height, depth slices or MSAA samples differ.
  - `NextPassReadsTexture`: "The next pass reads data output by this pass as a regular texture". Sampling something written inside the open native pass ends it. Reading it through `SetInputAttachment` does not.
  - `NextPassTargetsTexture`: the next pass renders into a texture that an earlier subpass sampled.
  - `NonRasterPass`: compute or unsafe work ends the native pass.
  - `DifferentDepthTextures`: "we only allow one [depth] in a whole NRP".
  - `AttachmentLimitReached` (`FixedAttachmentArray.MaxAttachments`, 8) and `SubPassLimitReached` (`k_MaxSubpass = 8`, "Needs to match with RenderPassSetup.h").
  - `FRStateMismatch` (foveation), `DifferentShadingRateImages/States`, `MultisampledShaderResolveMustBeLastPass`, `ExtendedFeatureFlagsIncompatible`, `BackbufferInMultipleRenderTargetsNotSupported`, `PassMergingDisabled`, `EndOfGraph`, `Merged`.
- If a writer pass was culled, reading its output does not end the native pass (a culled writer writes nothing).
- **Subpass reuse**: if a merged graph pass uses exactly the same attachments as the previous subpass, it goes into that **same native subpass** instead of starting a new one, "because nextSubpass is expensive on some platforms (even if its' essentially a no-op as it's using the same attachments)". A raster pass with zero attachments (for example one that only sets globals) merges without ending anything ([PassesData.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/Compiler/PassesData.cs) `TryMergeNativeSubPass`). A subpass that has depth bound only because of merging gets a matching read-only-depth flag.
- **Load/store are inferred and audited** (`LoadAudit`/`StoreAudit`):
  - Load reasons: `LoadImported`, `LoadPreviouslyWritten`, `ClearImported`, `ClearCreated`, `FullyRewritten`.
  - Store reasons: `StoreImported`, `StoreUsedByLaterPass`, `DiscardImported`, `DiscardUnused`, `DiscardBindMs`, `NoMSAABuffer`.
  - Each reason has a human-readable message that the viewer displays.
- **Memoryless detection** (`DetectMemoryLessResources`): if the platform supports memoryless textures, any non-imported, non-global texture that is created **and** destroyed inside one native render pass is marked memoryless. The reasoning in the code: a texture cannot be used as a plain texture partway through a merged pass, or the passes would never have merged.
- **Async compute**: resource release is postponed to "the first pass on gfx queue waiting for a fence" (`FindFirstPassIdOnGraphicsQueueAwaitingFenceGoingForward`).
- **Compilation caching**: when `enableCompilationCaching` is set, the graph is hashed each frame (`ComputeGraphHash`, FNV-1a over every pass's `ComputeHash`) and the compiled result is reused on a hit ([RenderGraph.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraph.cs)).

### 2.4 Input attachments and framebuffer fetch

`builder.SetInputAttachment(tex, idx, AccessFlags.Read)` together with shader `FRAMEBUFFER_INPUT_X_HALF/FLOAT/INT/UINT` and `LOAD_FRAMEBUFFER_X_INPUT(idx, positionCS.xy)` merges the writer and reader into one native render pass. That is Vulkan subpasses on Vulkan and framebuffer fetch on Metal. Other APIs fall back to copies through memory. ([Framebuffer fetch](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-framebuffer-fetch.html))

### 2.5 Resource pooling

There is no heap-level placed aliasing in core. `RenderGraphResourcePool` keeps real textures keyed by **descriptor hash** in a sorted list. Released textures go back to the pool and are reused by later passes in the same frame with an identical descriptor (`IntraFrameMemoryAliasing = true` by default, turned off for the Frame Debugger), and stale entries are purged ([RenderGraphResourcePool.cs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphResourcePool.cs)). Mobile gets its big win from memoryless attachments, not heap aliasing.

### 2.6 Multithreading

Recording happens inside the graph's execute loop on the render thread through Unity's command buffers. The graph compiler contains no parallel-recording code. Parallelism comes from Unity's native graphics jobs below the SRP layer (grep of [RenderGraph.cs / NativePassCompiler.cs](https://github.com/Unity-Technologies/Graphics/tree/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph)).

### 2.7 Render Graph Viewer

It shows a pass × resource grid of access blocks. Red is write, green is read, grey is no access, and a dotted line means the resource does not exist yet. Blue bars mark merged native passes. Selecting a pass shows "Native Render Pass Info" and **"Pass break reasoning … why URP could not merge this render pass with the next"**, the attachment load/store actions with their reasons, and each resource's Size, Format, Clear, BindMS, Samples and **Memoryless** status. Filters cover culled, raster, unsafe and compute passes, and imported resources, textures, buffers and acceleration structures. ([Viewer](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-view.html), [Viewer reference](https://docs.unity3d.com/6000.3/Documentation/Manual/urp/render-graph-viewer-reference.html))

### 2.8 Pain points

- Boilerplate: one user needed "50 lines of code…to implement 'get a named asset that blits a material to screen'". Unity promised high-level wrappers ([forum](https://discussions.unity.com/t/introduction-of-render-graph-in-the-universal-render-pipeline-urp/930355)).
- Global shader state had to be patched in after the fact, and it disables culling.
- The merger is greedy and never reorders, so a single compute pass between two raster passes splits a native pass (`NonRasterPass`). Authors have to order passes by hand to get merges.
- Migrating everything took about three releases (2023.3 → 6.4).

---

## 3. Godot 4: RenderingDeviceGraph (4.3+)

### 3.1 Design

- The graph lives at the lowest level, inside `RenderingDevice`. "Its construction is completely invisible to the programmer using RenderingDevice." "No changes are expected from users whatsoever." ([Godot blog](https://godotengine.org/article/rendering-acyclic-graph/)) Merged 2024-01-09 in [PR #84976](https://github.com/godotengine/godot/pull/84976).
- Commands are serialised into a byte stream, and the graph reorders them "based on the dependency between the resources used … to be processed as early as possible" ([PR #84976](https://github.com/godotengine/godot/pull/84976)).
- **Dependency inference.** Each resource has a `ResourceTracker` holding the last writer and the list of reader commands. Reads of the same resource can run concurrently. A write depends on every earlier read and write. **A layout change counts as a write even when the access is read-only.** Resources created with initial data and no mutation flags have no tracker, and one is created later if they turn out to be modified. ([Godot blog](https://godotengine.org/article/rendering-acyclic-graph/)) Usage is a closed enum: `COPY_FROM/TO, RESOLVE_FROM/TO, UNIFORM_BUFFER_READ, INDIRECT_BUFFER_READ, TEXTURE_SAMPLE, STORAGE_IMAGE_READ(_WRITE), ATTACHMENT_COLOR_READ_WRITE, ATTACHMENT_DEPTH_STENCIL_READ_WRITE, …, ACCELERATION_STRUCTURE_READ(_WRITE)` ([rendering_device_graph.h](https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_graph.h)).
- **Slices** (mips and layers) track layouts separately on a "dirty list". Using the parent texture "normalizes" them. "It is never possible for one command to use slices of the same texture that overlap" ([blog](https://godotengine.org/article/rendering-acyclic-graph/)).
- **Coarse nodes.** A draw list or compute list is **one node**. Nothing is reordered inside it: "There is no benefit to allowing reordering within these structures". A frame comes out at about 300 nodes instead of hundreds of thousands ([blog](https://godotengine.org/article/rendering-acyclic-graph/)).
- **Levels.** A topological sort produces "levels" of mutually independent commands. Within a level, commands are grouped by type (copy, draw, compute) as a secondary sort, and all barriers for a level are submitted together before it. The result was a "60-80% [reduction] of the total amount of barrier calls in a frame" ([PR #84976](https://github.com/godotengine/godot/pull/84976)).
- **Results**: roughly 5-15% frametime improvement and "No performance regressions … as far as GPU performance". About 2,500 lines of manual sync code were deleted. CPU cost went up about 10% in heavy scenes, but "Graph construction and topological sorting don't even account for more than 1% of the CPU time". Most of the overhead is serialisation and Vulkan calls ([blog](https://godotengine.org/article/rendering-acyclic-graph/)).
- The API got simpler: "Barrier bitmasks are gone"; draw lists "no longer need to specify storage textures" ([PR #84976](https://github.com/godotengine/godot/pull/84976)).

### 3.2 Load/store inference (4.4)

[PR #98670](https://github.com/godotengine/godot/pull/98670), merged 2024-11-27, removed the need for users to specify initial and final actions. The graph infers load, clear and discard after reordering, and render-pass objects are created only once the graph knows the actions. A `TextureBehavior` hint (such as `DISCARD_BETWEEN_FRAMES`) lets users override. Measured effect: "a completely blank scene that uses 200% resolution scale with MSAA 4X on a Mali-G715 has improved from 52 FPS to 120 FPS … just from being able to discard any writes to MSAA buffers". The PR also sets up later support for true transient attachments.

### 3.3 Multithreaded recording

- Draw lists suit secondary command buffers, because "their contents do not need to be reordered, but their location in the primary command buffer is determined during the topological sorting step". The graph "can automatically issue secondary command buffers and record them on background threads when they reach an arbitrary size threshold". This is **off by default**: "Secondary command buffers can run into some strange issues on different hardware", including NVIDIA editor crashes ([blog](https://godotengine.org/article/rendering-acyclic-graph/)).
- In current master, `#define SECONDARY_COMMAND_BUFFERS_PER_FRAME 0` in [rendering_device.cpp](https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device.cpp) keeps it off. The live mechanism is `split_cmd_buffer` on a draw list, which ends the current **primary** command buffer and continues on a fresh pooled one with a semaphore (used for the swapchain blit) ([rendering_device_graph.cpp](https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_graph.cpp)).
- Async compute and multi-queue are future work. "Command queue submissions are not free" ([blog](https://godotengine.org/article/rendering-acyclic-graph/)).
- There is no transient aliasing inside the graph. Resources are ordinary allocations.

### 3.4 Subpasses in Godot's Mobile renderer

Subpasses are authored explicitly: `framebuffer_format_create_multipass` plus `draw_list_switch_to_next_pass` (the graph records `add_draw_list_next_subpass`). The Mobile renderer "use[s] sub-passes whenever possible". Subpasses cannot "read neighboring pixels", so glow, DOF, screen-texture reads and depth-texture reads force a full write-out, and "a mix of sub-passes and normal passes are used … [with] a notable performance penalty" ([internal rendering architecture](https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/internal_rendering_architecture.rst)). Mobile also defaults to RGB10A2 instead of RGBA16F to halve bandwidth.

### 3.5 Criticism

Debugging is hard: "it can be very tough to produce a usable backtrace as the context that generated the commands is long gone" ([blog](https://godotengine.org/article/rendering-acyclic-graph/)). The graph has no whole-frame knowledge above the command level, so it cannot cull, alias memory, or merge subpasses on its own. It optimises ordering and barriers only.

---

## 4. Frostbite FrameGraph (Yuriy O'Donnell, GDC 2017)

Sources: [GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in), [slides](https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495).

- **Motivation.** The WorldRenderer "organically grew from 4k to 15k SLOC", with "single functions with over 2k SLOC". After the refactor it was about 5K SLOC. BF-era frames "typically see few hundred passes and resources".
- **Three phases.** *Setup* declares passes and their reads, writes and creates. *Compile* culls unreferenced resources and passes, computes lifetimes, and allocates concrete resources ("acquire right before first use, release after last use"). It also derives bind flags from actual usage. *Execute* calls immediate-mode callbacks.
- **API.**
  ```cpp
  frameGraph.addCallbackPass<PassData>("MyRenderPass",
    [&](RenderPassBuilder& builder, PassData& data) {    // setup
        data.input  = builder.read(input);
        data.output = builder.createTexture("Out", desc);   // + write()/useRenderTarget()
    },
    [=](const PassData& data, const RenderPassResources& res, IRenderContext* ctx) { /* execute */ });
  ```
  Lambdas were chosen to "minimize migration friction". Code flow stays top to bottom.
- **Culling** uses reference counts (flood fill from outputs). Disconnecting a debug output automatically turns off every pass upstream of it.
- **Transient resource system.** Resources live "no longer than one frame". PS4 uses virtual-memory aliasing. D3D12 PC uses placed resources in several small heaps, which leads to a "fragmented address space". Xbox One uses physical aliasing with ESRAM. D3D11 uses object pools. Aliasing needs correct metadata state (FMASK/CMASK/DCC), a `DiscardResource` before first use, and aliasing barriers.
- **Memory numbers.** At 720p, 147 MB without aliasing versus 80 MB on DX12 PC, 77 MB on PS4, and 76 MB on Xbox One (32 ESRAM + 44 DRAM). At 4K on DX12, 1042 MB versus 472 MB, **saving 570 MB**.
- **Async compute** is one line, `builder.asyncComputeEnable(true)`, and it moves the pass and its child passes to the async queue. Lifetimes extend to the sync point. Placement is still manual. Riccardo Loggini notes that UE and 2017 Frostbite "do not compute the minimal amount of fences, and put manual fences where they want to optimize workflow" ([Loggini](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)).
- **Modules and the blackboard.** There are stateless free functions whose inputs and outputs are graph handles, and persistent modules that own history or LUTs. Modules communicate through a **blackboard**, a type-ID-keyed hash table that allows "controlled coupling" ([slides](https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495), [gfx-rs frame graph notes](https://github.com/gfx-rs/gfx/wiki/Frame-graphs)).
- **Future work** listed in the talk: global barrier optimisation, async compute bookmarks, profile-guided async and memory placement.
- **Why it was influential.** The graph is rebuilt every frame from code, so conditional features are just `if` statements. The setup/execute lambda pair became the template everyone copies. Transient aliasing produced a large, measurable win. The talk also made the case that a graph is a *modularity* tool as well as an optimiser.

---

## 5. Others, briefly

### 5.1 O3DE Atom: FrameScheduler / FrameGraph

- **ScopeProducer** has three virtuals: `SetupFrameGraphDependencies(FrameGraphInterface)` declares attachments, `CompileResources(FrameGraphCompileContext)` builds SRGs and descriptors once real views exist, and `BuildCommandList(FrameGraphExecuteContext)` records ([ScopeProducer.h](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/Code/Include/Atom/RHI/ScopeProducer.h)). The separate *compile-resources* phase is a notable split: descriptor and SRG work runs after allocation and before recording, so recording threads see finished bindings.
- **Attachments** are either imported (persistent) or transient (created by the scheduler and valid only for the scopes that use them). Registration is global by ID, so downstream scopes can use an attachment "without caring where it came from". Every scope must still `Use*` an attachment explicitly, even one it created. There is a `UseSubpassInputAttachment` for "pixel local load operations". Explicit edges exist (`ExecuteAfter/Before`), along with fence signal/wait. `SetEstimatedItemCount` load-balances a scope "across command lists. A small value may result in the scope being merged onto a single command list, whereas a large one may result in the scope being split across several", and `SetHardwareQueueClass` picks the queue ([FrameGraphInterface.h](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/Code/Include/Atom/RHI/FrameGraphInterface.h)).
- **Execute groups.** On hierarchical-command-list APIs (Vulkan, Metal) a group can hold one primary command buffer with parallel secondary children for a single scope, or one command list shared serially across several scopes ([FrameGraphExecuteGroup.h](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/Code/Include/Atom/RHI/FrameGraphExecuteGroup.h)). A root Graphics scope always runs first, for pool uploads and so that every attachment chain starts on graphics. Statistics include a transient-heap × scope-timeline grid "useful when visualized to show overlap" ([FrameScheduler.h](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/Code/Include/Atom/RHI/FrameScheduler.h)).
- **Vulkan backend** ([Vulkanised 2024, Akio Gaule](https://vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-akio-gaule-o3de.pdf)): creation is serial, compile is partly parallel, execution is parallel. Pipeline stage, access flags and layouts are **deduced from the usage type**, and subresource layouts are tracked in an interval map. It emits cross-queue semaphores plus queue-ownership release/acquire barriers, and aliasing barriers on first use. Barrier optimisation merges memory barriers, merges same-subresource barriers, and "Transform barriers into subpass dependencies if possible / Use initial and final layout of VkAttachmentDescription". Per pass the emission order is aliasing → clear → prologue → work → epilogue → resolve. Listed future work: "Visual Debugging tools".
- **Subpasses are opt-in and authored.** A parent pass sets `"MergeChildrenAsSubpasses"` and `RPI::ParentPass` builds a single `RenderAttachmentLayout` for its children. The RFC documents a real Vulkan trap. PSOs are created against one `VkRenderPass` and used inside another, and the two must be *compatible*, so "if there are Subpass Dependency declarations, those declarations must be identical". O3DE had to unify two code paths into a single `SubpassDependencyHelper` ([RFC: Subpasses support in RPI](https://github.com/o3de/sig-graphics-audio/blob/main/rfcs/SubpassesSupportInRPI/RFC_SubpassesSupportInRPI.md)).

### 5.2 EA SEED Halcyon (Graham Wihlidal, 2018)

Source: [Halcyon Architecture "Director's Cut" PDF](https://media.contentapi.ea.com/content/dam/ea/seed/presentations/wihlidal-halcyonarchitecture.pdf), [blog](https://www.wihlidal.com/blog/graphics/2018-11-30-halcyon-architecture/).
- **Render handles**: 64-bit, generational (catches double deletes and use after delete), type-safe, serialisable, with constant-time lookup. One handle can map to a *different* backend object on each device (mGPU, and even DX12 and Vulkan in the same process).
- **Render commands**: a high-level, **stateless** command list that is "parallel recording" friendly, tracks the queue types it encounters, and checks specs (for example no draws on compute). It is then "compiled" to the low-level API with "Perfect redundant state filtering" and "compile once, submit multiple times".
- **Render graph** ("inspired by FrameGraph"): automatic transient resources, imports, transitions, render-target batching, DiscardResource, aliasing barriers. "No concept of a 'frame'", and graphs compose at different frequencies. "Fully automatic transitions and split barriers". Construction is "Serial operation (by design)", and evaluation is "Highly parallelized". Data flows between passes through **nested scopes** (`scope.get<T>()` over POD structs, with shadowing).
- **Honest caveats**: "Fine grained memory reuse sub-optimal with current PC drivers / Lose ~5% on aliasing barriers and discards". Automatic queue scheduling is "Ongoing research … Not enough to specify dependencies" and needs duration and bottleneck heuristics. It has automatic per-pass GPU and CPU profiling and a live debug overlay showing evaluated passes, their inputs and outputs, and **resource versions**.

### 5.3 Activision Task Graph Renderer (REAC 2023)

Source: [slides with speaker notes](https://enginearchitecture.realtimerendering.com/downloads/reac2023_task_graph_renderer.pdf), [publication page](https://research.activision.com/publications/2023/06/Task-Graph-Renderer-at-Activision). This is the most useful industrial retrospective in the set.
- **API evolution.** A code-driven class per task was rejected for its duplication. The team moved to a **declarative macro DSL**: `BEGIN_TASK(X) COLOR_TARGET_CREATE(...) TEXTURE_READ(src) CONDITION_FUNC(f) END_TASK()`. One definition generates the access table, a handle struct (`X_Struct`), and a variadic function `cX(builder, handles...)` for the setup DSL. Lua hot reload was prototyped but not shipped.
- **Two-tier compile.**
  - Heavyweight, at level load (10-100+ ms): run `RendererSetup` with level and platform constants, build dependencies from external resources backwards (which culls implicitly), schedule into GPU order, and create heaps and place resources, including every dynamic-resolution variant.
  - Lightweight, per frame (0-1 ms): evaluate `CONDITION_FUNC`s into a bitfield key, then build or fetch a cached **permutation** of barriers, syncs and render passes. There are 5-10 conditions (more than 1000 permutations), and **no per-frame allocation**. Memory is sized for the worst case.
- **First-class temporal resources** (`COLOR_TARGET_CREATE_TEMPORAL(..., 2)`) rotate automatically and are nulled on camera cuts. Split-screen "just works" because the system multiplies temporal copies per view. **Null resources** let one uber-definition cover optional features. Subresources are addressed with `handle.Mip(i)`.
- **Results (2019)**:
  - 236 tasks and 258 resources.
  - Memory saved: about 100 MB on average, up to 500 MB.
  - About 25% less CPU render time. GPU time was initially the same; later, two independent pipelines were interleaved behind one merged barrier.
  - Roughly 6000 lines of glue code across 30 files became about 1500 lines, and the whole pipeline became readable in one file.
- **Later work.**
  - *Temporal resource aliasing*: transients go into the unused window of the current temporal layer, with the layout ping-ponging every frame.
  - *Mobile* (Metal including tile shaders, and Vulkan): the scheduler "prefer[s] placing render pass compatible tasks consecutively" to batch tasks into one API render pass, and runs multi-pass fragment variants instead of single-pass compute.
  - *ImGui live debug UI*: heap layouts, task and event viewer, before/after resource capture, dependency graph viewer.
- **Multithreading lessons.**
  - Their earlier draw-list threading "wasn't viable on Tiled Based Deferred Rendering hardware", because splitting long draw lists across command buffers costs performance.
  - The new design: *work queues* per hardware queue, owned by one thread; queues split into *task batches* tagged thread-safe or not; batches balanced across workers; jobs issued **in GPU order**. The owning thread submits and handles non-thread-safe tasks. If workers fall behind or run out of linear-allocator memory, their work is **handed off or pre-empted** to the render thread, which has a fenced ring allocator. Tasks had to be annotated and made thread-safe one at a time.

### 5.4 Ubisoft Anvil

Anvil has a Frame Graph giving "full control over lifetime and usage of a big portion of render resources" with "automatic and optimized resource transitions, sync fences and memory allocations". It was presented at GDC 2017 in "Moving to DirectX 12: Lessons Learned" (Tiago Rodrigues) ([GDC Vault](https://www.gdcvault.com/play/1024656/Advanced-Graphics-Tech-Moving-to), summary in [Loggini](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)). No public API details beyond that.

### 5.5 AMD Render Pipeline Shaders (RPS) SDK

Sources: [GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders), [RPS 1.0 intro](https://gpuopen.com/learn/rps_1_0/), [tutorial](https://gpuopen.com/learn/rps-tutorial/rps-tutorial-part1/), [rps_runtime.h](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/blob/main/include/rps/runtime/common/rps_runtime.h).
- **RPSL** is HLSL extended with attributes. Nodes are *declared with access signatures*, and the graph is a function:
  ```hlsl
  graphics node Triangle([readwrite(rendertarget)] texture rt : SV_Target0);
  node Upscale([readwrite(rendertarget)] texture dst : SV_Target0, [readonly(ps)] texture src);
  export void main([readonly(present)] texture backbuffer) {
      texture off = create_tex2d(backbuffer.desc().Format, w/2, h/2);
      clear(off, float4(0,0.2,0.4,1)); Triangle(off); Upscale(backbuffer, off);
  }
  ```
  C++ binds callbacks: `rpsProgramBindNode(entry, "Triangle", &DrawTriangleCb, this)`. RPS binds render targets from `SV_Target[n]` and sets the default viewport and scissor.
- **Runtime compiler**: resolve dependencies from node signatures, insert transition nodes, build the DAG, schedule, then the backend creates heaps, resources, descriptors and framebuffers. Scheduler flags include `KEEP_PROGRAM_ORDER`, `PREFER_MEMORY_SAVING` ("minimizing transient resource lifetimes and aggressive aliasing. This may increase the number of barriers"), `RANDOM_ORDER` (for testing), `MINIMIZE_COMPUTE_GFX_SWITCH`, `DISABLE_DEAD_CODE_ELIMINATION`, and `WORKLOAD_TYPE_PIPELINING_*`. `ALLOW_SPLIT_BARRIERS`, `AVOID_RESCHEDULE`, `ALLOW_FRAME_OVERLAP` and `PREFER/DISABLE_RENDERPASS_TRANSITIONS` are marked "Reserved for future use" (not implemented).
- **Multi-threaded recording**: `rpsRenderGraphGetBatchLayout` returns per-queue `RpsCommandBatch`es with wait and signal fences. `rpsRenderGraphRecordCommands` takes `cmdBeginIndex/numCmds`, so an application can record command ranges on separate threads. Inside a node, `rpsCmdCloneContext` plus `rpsCmdBeginRenderPass/EndRenderPass` support "RenderPass suspend / resume & secondary command buffer behaviors". Diagnostics can dump the DAG and the pre- and post-schedule state, and a visualiser library shows the heap layout.
- **Backends**: D3D12, Vulkan and D3D11. **There is no Metal backend.** The last commit is "Open Beta 1.1.1 Maintenance Release" on 2024-05-14, so the project is effectively dormant ([commits](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/commits/main)).

---

## 6. Comparison table

| | UE RDG | Unity RenderGraph (NRP) | Godot RD graph | Frostbite FG | O3DE Atom | Activision TG | AMD RPS |
|---|---|---|---|---|---|---|---|
| Level | High-level, per scene render | SRP level, per camera | **Inside the RHI**, per command | High-level, per frame | RHI FrameScheduler + RPI passes | High-level DSL | Separate DSL + runtime |
| Declaring deps | Reflected param struct (same struct binds shader params) | Builder calls per pass, typed pass kinds | **Inferred** from every RD call | Builder `read/write/create` in a setup lambda | `Use*Attachment` per scope, global IDs | Macro access tables | Node signature attributes |
| Rebuilt when | Every frame | Every frame, **compile cached by hash** | Every frame (implicit) | Every frame | Every frame | **Level load, plus per-frame cached permutations** | On update, schedule reusable |
| Culling | Yes (roots = extracted/NeverCull) | Yes (side effects / unused writes) | No | Yes (refcount) | Orphan removal | Yes (backwards from externals) | DCE (flag) |
| Reordering | Mostly program order | **None**, greedy linear | Topological levels + type grouping | Program order | Topological sort | Scheduler; mobile prefers RP-compatible adjacency | Full scheduler with policies |
| Barriers | Split, batched, per-subresource | Derived by compiler | Batched per level (-60-80%) | Derived | Deduced from usage, merged, folded into subpass deps | Batched, merged | Generated; split barriers "reserved" |
| RP / subpass merging | Merge **identical** contiguous RTs only; mobile subpasses hand-written with `SubpassHint` + `NextSubpass` | **Automatic subpasses** + input attachments + inferred load/store + **memoryless** | Explicit multipass framebuffers | Not covered in the talk | Explicit `MergeChildrenAsSubpasses` | Batch compatible tasks into one API RP | RP per node; RP transitions "reserved" |
| Transient aliasing | Heap aliasing (platform-dependent) | Descriptor-hash texture pool + memoryless | None | Heap/VM aliasing (big win) | Transient heap aliasing | Heaps placed at load, temporal-gap aliasing | Aggressive heap aliasing |
| Async compute | Manual per-pass flag, auto fences | `EnableAsyncCompute`, auto fences | Not yet | Manual flag, auto lifetime | Queue class per scope | Tag per task, own worker thread | Scheduler, multi-queue batches |
| MT recording | Parallel spans of passes → cmd lists (**off on mobile**) | Not in the graph | Secondary CBs **off by default** (driver issues) | n/a | Scope split across parallel cmd lists via item estimate | Batches in GPU order, hand-off/pre-empt | Range recording + cloned contexts |
| Tooling | RDG Insights, immediate mode, transition log, cvars per optimisation | **Viewer with merge-break and load/store reasons** | Print macros only | Graph visualisation | Heap × timeline stats | Live ImGui debugger, heap/graph/resource capture | DAG dumps, visualiser |

---

## 7. Lessons for a lean design

### 7.1 Copy these

1. **Setup and execute as two lambdas (or a declaration plus a record function), rebuilt from code every frame.** Every engine converged on this (Frostbite, UE, Unity, Activision). Conditional features stay ordinary `if`s. Keep execute callbacks side-effect free, so parallel recording can be added later without rewriting passes (UE's rule).
2. **Typed pass kinds whose command-encoder types match what the compiler must know.** Unity's lesson: a raster pass gets an encoder that *cannot* change render targets, so attachments are fully declared and the compiler can form native passes. Compute gets a compute encoder. Keep **one** escape hatch ("unsafe/external") that deliberately ends merging and is easy to grep. This maps directly onto Metal (render, compute and blit encoders) and Vulkan (render pass or dynamic rendering scopes).
3. **Separate attachment declarations from sampled reads, and add an explicit input-attachment read.** Unity's `SetRenderAttachment` / `UseTexture` / `SetInputAttachment` split is exactly the information needed to decide "same tile memory or not". A sampled read of something written inside an open native pass ends it. An input-attachment read keeps it on chip.
4. **Infer load/store and memoryless, and never make users write them.** Unity (`LoadAudit`/`StoreAudit` plus `DetectMemoryLessResources`) and Godot 4.4 (inferred actions, Mali MSAA 52 → 120 FPS) show this is the largest TBDR win, and it is purely derived data. The memoryless rule is simple: a non-imported texture whose whole lifetime sits inside one native pass is memoryless (`MTLStorageModeMemoryless` / `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` + `TRANSIENT_ATTACHMENT`).
5. **Record the reason for every compiler decision.** Unity's `PassBreakReason` enum with human-readable messages, together with load/store reasons, is the cheapest and most valuable visualisation feature. Put the reasons in a graph dump (DOT/JSON) before building any UI. Also add a cvar to switch off each optimisation (UE: cull, merge, parallel, alias, async) for bisection, plus an immediate or serial mode.
6. **Derive stages, access and layouts from a small closed usage enum** (Godot's `ResourceUsage`, O3DE's "VkPipelineStageFlags/VkAccessFlags/VkImageLayout are deduced from the resource usage"). Pass authors never write barriers.
7. **Coarse nodes.** Godot keeps a draw or compute list as one node, which gives about 300 nodes per frame and makes graph cost negligible (under 1% CPU). A lean graph works per pass and never per draw.
8. **Handles are generational indices; resources are virtual until compile** (Halcyon render handles, UE/Unity handles). Import and export are explicit (UE `RegisterExternal` / `QueueExtraction`, O3DE imported vs transient). History and temporal resources should be **first-class**, with automatic rotation and reset on camera cuts (Activision), instead of an extract/re-import dance every frame.
9. **Cache compilation.** Unity hashes the graph and reuses the compiled result. Activision splits compile into a heavy stage at level load and cheap per-frame cached permutations keyed by a condition bitfield. For a small renderer, "hash the declared graph → reuse the compiled schedule, barriers and render-pass objects" is the sweet spot. It also fixes the Vulkan render-pass and PSO compatibility churn.
10. **Parallel recording by contiguous spans in GPU order, merged back in submission order** (UE ParallelExecute spans, Activision task batches). One primary encoder per span is simpler and more portable than secondary command buffers. On Metal, use `MTLParallelRenderCommandEncoder` only *inside* a single native pass. On Vulkan, prefer multiple primary command buffers per span, and use secondaries only inside a render pass that is known to be large (a Godot-style size threshold).

### 7.2 Avoid these

1. **Fusing shader-parameter reflection with dependency declaration** (UE's `BEGIN_SHADER_PARAMETER_STRUCT`). It is compact, but unused parameters create false dependencies (`ClearUnusedGraphResources` exists to patch that), the macros are heavy, and template errors are opaque. Keep "what I access" and "how I bind it" as separate concepts, even if a helper fills both.
2. **Global shader state as an implicit channel.** Unity had to add `AllowGlobalStateModification`, which kills culling. Pass every resource through the graph.
3. **A fully implicit, RHI-level graph as the *only* graph** (Godot). It works well for barriers and needs no API, but it cannot cull, cannot alias, and cannot build subpasses. Its CPU cost comes from serialising every command, and backtraces are lost. Automatic barrier batching is still worth borrowing inside the executor, but whole-frame knowledge has to come from pass declarations.
4. **Greedy, order-preserving merging with no reordering at all** (Unity). One compute pass in the wrong place splits a native pass. A lean design can stay simple but should add one cheap reordering heuristic, as Activision does on mobile: move a render-pass-compatible raster pass next to its compatible neighbour when no dependency forbids it.
5. **Hand-authored subpass chains or hints** (UE `ESubpassHint` + `NextSubpass`, O3DE `MergeChildrenAsSubpasses`). They exist because those graphs were built for desktop first and retrofitted for TBDR. If tile memory is a goal from day one, infer subpasses as Unity does. If authoring is needed at all, the only input should be "this read is an input attachment".
6. **Secondary command buffers as the default parallelism path.** Godot ships them disabled after IHV crashes. Activision found draw-list splitting "wasn't viable on TBDR hardware". UE disables parallel execute on mobile altogether. Parallelism across passes (spans) should come first, and parallelism inside a pass only where the pass is big.
7. **Automatic async-compute scheduling.** Halcyon calls it "ongoing research … not enough to specify dependencies". UE, Frostbite and Unity all use a manual per-pass flag with automatic fences and lifetime extension. Copy that, and make it a no-op on Apple and Adreno until profiling shows a win.
8. **Fine-grained heap aliasing as an early priority on PC.** Halcyon lost about 5% to aliasing barriers and discards. Frostbite's big numbers came from consoles with ESRAM and virtual memory. On Apple and Android the larger win is memoryless attachments. A descriptor-keyed pool with reuse in the same frame (Unity's approach) is a sound first step. Placed-heap aliasing (Metal `MTLHeap` + `makeAliasable`, Vulkan shared `VkDeviceMemory`) can come later, behind a flag that turns it off.

### 7.3 Present for legacy reasons only

- UE: `UseExternalAccessMode`, `SkipTracking`, `SkipRenderPass`, `NeverParallel`, `FRHICommandListImmediate` passes, `ConvertToExternalTexture`. These exist for non-RDG code. The dual pooled-RT and transient allocator paths exist for platform coverage.
- Unity: `AddUnsafePass` (the full `SetRenderTarget` world), the obsolete `AddRenderPass` and `nativeRenderPassesEnabled`, global textures, Compatibility Mode (removed in 6.4). The lesson is that the dual path cost about three releases, so a clean-slate design should not keep an "old path" switch.
- Godot: `draw_list_begin_split`, the old explicit initial/final actions (now inferred), and the swapchain `split_cmd_buffer` special case.
- O3DE: the two VkRenderPass creation paths that had to be unified for subpass-dependency compatibility. With `VK_KHR_dynamic_rendering` + `VK_KHR_dynamic_rendering_local_read`, a new Vulkan backend can avoid render-pass-object compatibility entirely (worth verifying against Adreno driver support in a separate study).

### 7.4 Minimal feature set this research supports

1. Pass kinds: `Raster{attachments, sampled reads, input-attachment reads}`, `Compute{reads, writes, async flag}`, `Copy`, `External` (ends merging, fully barriered).
2. Compile pipeline: validate → build versioned resource graph → cull from imported/exported roots → (optional) cheap reorder for merge adjacency → form native passes and subpasses with **break reasons** → infer load/store with **reasons** → mark memoryless → lifetimes → derive barriers from usage enums, batched per boundary → spans for parallel recording.
3. Executor: per span, one command encoder or command buffer recorded on a worker, submitted in graph order. Barriers are emitted between passes. Async compute is a per-pass flag with fork/join fences, disabled per platform.
4. Resources: generational handles, a transient pool keyed by descriptor, memoryless when the lifetime fits inside a native pass, first-class history/temporal resources, explicit import/export.
5. Tooling from the first day: JSON/DOT dump of passes, resources, access, native-pass groups, break reasons, load/store reasons, memoryless flags and lifetimes; a cvar per optimisation; a serial/immediate debug mode; a compile cache keyed by graph hash.
