# Sparkle renderer layer: current frame structure (evidence map for render-graph design)

All paths are relative to `/Users/qingtian.li/Projects/Sparkle`. I read the code only. Nothing was built or run. Anything marked "appears" is a static reading that has not been checked by running.

## 1. Frame loop and threading

**Main thread** (`AppFramework::MainLoop`, `libraries/source/application/AppFramework.cpp:295-409`), in order:

1. It enqueues a render-thread task that copies the frame's `RenderConfig` snapshot: `RenderFramework::NewFrame` (:309-311).
2. It runs its own pending tasks, ticks the view and input, handles scene-load completion, and fires `NotifySceneLoaded` (:354).
3. It runs `Scene::Tick`/`ProcessChange` (:378-379). Scene components push proxy changes to the render thread as closures through `TaskManager::RunInRenderThread` (for example `PrimitiveComponent.cpp:44,76`, `CameraComponent.cpp:145`, `SkyLight.cpp:311,460`).
4. It builds the ImGui UI on the main thread (`UiManager::Render`, `UiManager.cpp:177-207`). The `ImDrawData` is cloned and handed over so the render thread never reads live ImGui state.
5. `RenderFramework::PushRenderTasks()` (:399). This pops the render `ThreadTaskQueue` into `tasks_per_frame_` and blocks while `tasks_per_frame_.size() >= MaxBufferedTaskFrames` (`RenderFramework.cpp:202-216`). `MaxBufferedTaskFrames = 1` (`RenderFramework.h:118`), so the main thread runs at most one frame ahead of the render thread.
6. If `app_config_.render_thread` is false, the main thread calls `RenderLoop()` itself (:401-404).

**Render thread** (`RenderFramework::RenderThreadMain` → `RenderLoop`, `RenderFramework.cpp:101-173`):

- `BeginFrame()` (:323-362):
  - `ConsumeRenderThreadTasks()` (:384-417) waits for and runs exactly one main-frame batch of closures. This is where proxies are added, removed or updated and where the config snapshot lands.
  - `UiManager::BeginRenderThread` sets `render_ui`.
  - `RecreateRendererIfNecessary()` (:278-321) runs. When the pipeline or `RenderResolution` changes, it does `WaitForDeviceIdle`, `Scene::RecreateRenderProxy`, destroys the renderer, `FlushDeferredDeletions`, then `Renderer::CreateRenderer`.
  - Then `rhi_->BeginFrame()`.
- `renderer_->Tick()` runs `SceneRenderProxy::Update` → `Renderer::Update()` (virtual) → `SceneRenderProxy::EndUpdate` (`Renderer.cpp:68-75`).
- `renderer_->Render()`.
- `ProcessScreenshotRequest()` arms a readback for the next frame's Render (:450-472).
- `EndFrame()` calls `rhi_->EndFrame()` (:364-382).

**RHI frame bracket** (`libraries/source/rhi/RHI.cpp`):

- `RHIContext::BeginFrame` (:208-237) runs the before-frame tasks, then `BeginFrameInternal`. On Vulkan (`VulkanContext::BeginFrame`, `VulkanContext.cpp:421-506`) that is the fence wait on this frame slot, swapchain acquire and `vkBeginCommandBuffer` on the slot's single primary command buffer. On Metal (`MetalContext.mm:67`) that is the semaphore wait and a new `MTLCommandBuffer`. BeginFrame then deletes the slot's deferred deletions and runs the slot's end-of-render tasks.
- `RHIContext::EndFrame` (:239-260) calls `EndFrameInternal`:
  - Vulkan: back-buffer transition to Present, `vkQueueSubmit`, `vkQueuePresentKHR` (`VulkanContext.cpp:510-579`).
  - Metal: `presentDrawable` then commit (`MetalContext.mm:88-110`).
  - After that come the end-of-frame tasks (screenshot PNG writes, IBL `Finalize`), `frame_index_ = (frame_index_+1) % max_frames_in_flight_`, and `render_target_pool_.Tick`.
- So the renderer records everything between acquire and submit, into **one command buffer per frame**. Present is not a renderer concern except for the final `ScreenQuadPass` into `GetBackBufferRenderTarget()`.

**Frames in flight:**

- Vulkan: equals the swapchain image count (`VulkanSwapChain.cpp:197`).
- Metal: equals `[view getMaxFrameInFlight]` (`MetalContext.mm:64`).
- Headless: 2 (`VulkanRHI.cpp:27`, `MetalContext.mm:9`).
- Per-slot state: `frame_stats_`, the timestamps in `RHIComputePass::execution_time_ms_`, deferred deletion buckets and end-of-render tasks.

**Proxies:**

- `SceneRenderProxy` (`SceneRenderProxy.h:21-161`) is render-thread-owned. It holds the primitive list, a one-frame `primitive_changes_` list (cleared in `EndUpdate`, `SceneRenderProxy.cpp:248-267`), the camera, sky and directional-light proxies, and `BindlessManager`.
- `Update` (:195-246) updates each proxy (mesh UBO uploads, sky IBL cooking, see section 5), marks the camera dirty on any change, and uploads bindless data. On the CPU pipeline it also rebuilds the CPU BVH.
- Renderer creation records `InitRenderResources` into a standalone command buffer, then waits idle (`Renderer.cpp:43-50`).

## 2. Per-renderer pass tables

Notation: "scene" is `RenderResolution::scene` = output × `render_scale`; "output" is `RenderResolution::output` (`RenderResolution.h:10-45`). The present target is the swapchain. In all rasterizing renderers, the tone-map target (`screen_color_rt_`/`tone_mapping_rt_`) is **B8G8R8A8Srgb @ output** and comes from `RHIRenderTargetPool`.

### Shared per-frame work in `Tick` (before `Render`, same command buffer)

These are GPU commands recorded during Update, not during Render:

- **IBL cooking** (Forward and Deferred only; `SkyRenderProxy.cpp:17-25` → `ImageBasedLighting::CookOnTheFly`, `ImageBasedLighting.cpp:173-219`): compute dispatches for BRDF LUT / diffuse / specular cubemaps. The number of steps per frame adapts to the last frame's GPU time (:221-254). On the first step it also runs clear render passes per cube face (`IBLDiffusePass.cpp:109-120`). This only runs while the IBL is not ready. It is skipped entirely when cooked artifacts load (`ImageBasedLighting.cpp:28-59`). When cooking completes, `IBLPass::Complete` enqueues `Finalize` as an **end-of-frame task that opens its own command buffer**, blits to fp16 and synchronously reads the result back to the CPU (`IBLPass.cpp:17-54,117-120`).
- **Acceleration structures**: the GPU renderer calls `tlas_->Build()/Update()` in `GPURenderer::Update` (`GPURenderer.cpp:373-385`). The Vulkan TLAS is recorded into the frame command buffer (`VulkanRayTracing.cpp:257`). The Vulkan BLAS uses a `OneShotCommandBufferScope` (:82-84). Metal uses an acceleration-structure encoder on the command buffer (`MetalRayTracing.mm:163`).
- UBO uploads for every pass (`UpdateFrameData`).

### ForwardRenderer (`ForwardRenderer.cpp`), `Render` at :132-216

| # | Pass | Type | Reads | Writes | Condition |
| --- | --- | --- | --- | --- | --- |
| 1 | `directional_shadow_pass_` (DepthPass) | raster, depth only | mesh VB/IB, light VP UBO | own D32 `shadow_map_resolution²` (default 1024), ClampToBorder | a directional light exists (lazy create/destroy in `HandleSceneChanges` :350-370) |
| 2 | `pre_pass_` (DepthPass) | raster, depth only | meshes, camera VP | own D32 @ scene, which *becomes* `scene_depth_` (:57-60) | `use_prepass` (default off) |
| 3 | `scene_color_pass_` (ForwardMeshPass "BasePass") | raster | shadow map, IBL BRDF/diffuse/specular, material textures, camera view UBO, prepass depth (SSAO) | `scene_color_` RGBA16F @ scene (clear) + `scene_depth_` D32 @ scene (clear, or load if prepass; depth test Equal) | always |
| 4 | `sky_box_pass_` | raster | sky cubemap (or IBL map override for debug) | `scene_color_` (load) + depth test LessEqual, depth store None | sky proxy with sky map |
| 5a | `tone_mapping_pass_` | raster fullscreen | `scene_color_` (bilinear clamp when upsampling, `ToneMappingPass.cpp:52-67`), exposure | screen color @ output | `output_image == SceneColor` |
| 5b | `texture_output_pass_` (ScreenQuadPass) | raster fullscreen | IBL BRDF LUT | screen color | `output_image == IBLBrdfTexture` (debug) |
| 6 | readback (no-UI) | copy | screen color | host staging buffer | screenshot requested without UI |
| 7 | `ui_pass_` | 3rd party (imgui), raster, LoadOp Load | cloned ImDrawData | screen color | `render_ui && !headless` |
| 8 | readback (with UI) | copy | screen color | staging | screenshot requested with UI |
| 9 | `present_pass_` (ScreenQuadPass) | raster fullscreen | screen color | back buffer (pre-rotation VS UBO) | always |

DAG: `shadow → base`, `[prepass →] base → skybox → tonemap|debugquad → [readback] → [ui] → [readback] → present`. The IBL cook (in Tick) feeds base and skybox. The `use_ray_tracing_` branch (:107-114) is dead code: `IsRayTracingMode()` means `pipeline == Gpu`, and the constructor asserts `Forward` (:25-27).

### DeferredRenderer (`DeferredRenderer.cpp`), `Render` at :111-208

| # | Pass | Type | Reads | Writes | Condition |
| --- | --- | --- | --- | --- | --- |
| 1 | shadow DepthPass | raster | meshes | own D32 shadow | directional light |
| 2 | `gbuffer_pass_` | raster MRT | meshes, materials, camera view UBO | `GBuffer::packed_texture` **RGBAUInt32 @ scene** (a single packed target, `GBuffer.cpp:7-24`) + `scene_depth_` D32 @ scene (both clear+store) | always |
| 3 | `directional_lighting_pass_` | raster fullscreen | packed GBuffer (`Load` at pixel), depth (nearest `Sample` at uv), shadow map, IBL maps, view UBO | `lighting_rt_` / `scene_color_` **RGBA16F @ scene** (pool; LoadOp None) | always |
| 4 | skybox | raster | sky cubemap | `scene_color_` (load) + `scene_depth_` (load, read-only test) | sky present |
| 5 | tonemap, or `debug_output_pass_` (BRDF LUT) | raster fullscreen | scene color | screen color @ output | `output_image` |
| 6-9 | readback / UI / readback / present | as in Forward | | | |

The renderer inserts the barriers by hand: GBuffer → Read (:136-138), depth → Read for lighting (:143-145), then back to DepthStencilOutput for the skybox (:150-152), scene color → Read (:159-161). DAG: `shadow → lighting`, `gbuffer → lighting → skybox → tonemap → [ui] → present`. `use_prepass` and `use_ssao` are ignored.

### GPURenderer: compute path tracer using inline ray query (`GPURenderer.cpp`), `Render` at :150-252

| # | Pass | Type | Reads | Writes | Condition |
| --- | --- | --- | --- | --- | --- |
| 0 | TLAS build/update (in Update) | AS build | BLAS | TLAS | primitive changes |
| 1 | `clear_pass_` (ClearTexturePass) | raster clear-only render pass, final layout StorageWrite | – | accumulator | `camera->NeedClear()` |
| 2 | path trace `ray_trace.cs.slang` (timestamped compute pass) | compute + ray query | TLAS, bindless textures/IB/VB-attrib, material buffers, sky map, UBO (camera, prev VP, seed, spp) | **accumulator** `scene_texture_` **RGBA32F @ scene** (read-modify-write running mean, `ray_trace.cs.slang:645-650`); when `write_gbuffer` is set, also the 6 `PathTracingDenoiserInputs` images | `tlas has instances && !accumulation_complete && !AccumulationPaused()` (:168) |
| 3 | `Denoiser::Encode` | compute + 3rd party | inputs + accumulator | provider output | `frame_denoiser_ && gbuffer_write_this_frame_` (:198-214) |
| 4 | tonemap | raster fullscreen | accumulator **or** denoiser output (`SetInput`, `ScreenQuadPass.h:63-71`). An RGBA32F source keeps nearest filtering even when upsampling. | `tone_mapping_rt_` @ output | always |
| 5-8 | readback / UI / readback / `screen_quad_pass_` present | | | | |

The denoiser inputs (`PathTracingDenoiserInputs.cpp`) are all @ scene with Texture|UAV usage:

- radiance+hitT: RGBA16F, or RGBA32F when `radiance_fp16` is off
- specular radiance+hitT: RGBA16F or RGBA32F, same switch
- normal+viewZ: RGBA32F
- albedo+objID: RGBA32F
- motion+hit/metallic: RGBA16F
- specular albedo+roughness: RGBA16F

They start as 1×1 dummies (:36-52) and are allocated lazily when a provider needs them (:54-76).

The effective input to tonemap depends on state:

- Denoiser off: `scene_texture_` (:277-280).
- Denoiser selected but not encoding this frame: `scene_texture_` (:194-197).
- Frozen or converged frame (no dispatch): tonemap keeps sampling whatever it was last pointed at. So **the denoiser output and the accumulator must persist across frames**.

DAG: `[clear] → trace → [pack → reblur → resolve | prepare → MetalFX → resolve] → tonemap → [ui] → present`.

### CPURenderer (`CPURenderer.cpp`), `Render` at :119-208

| # | Pass | Type | Notes |
| --- | --- | --- | --- |
| 1 | clear `frame_buffer_` | CPU, `ParallelFor` + wait | on NeedClear |
| 2 | `BasePass` | CPU worker tasks, one per row, `OnAll(...)->Wait()` (:419-450) | software path trace into `CPUGBuffer` |
| 3 | `DenoisePass` | CPU: optional spatial (ping-pong), then temporal accumulate into `frame_buffer_` (:506-545) | `spatial_denoise` |
| 4 | `ToneMappingPass` | CPU | into `Image2D` RGBA16F @ scene |
| 5 | upload + `CopyToImage` | copy | host buffer → `screen_texture_` (RGBA16F @ scene) |
| 6 | `upsample_pass_` (ScreenQuadPass, linear) | raster | only if `NeedUpsample()`, into composite @ output |
| 7-10 | readback / UI / readback / present | | |

The render thread blocks on the CPU work. The GPU part is only upload → [upsample] → UI → present.

## 3. Pass API

`PipelinePass` (`libraries/include/renderer/pass/PipelinePass.h:14-42`):

- It has three virtuals: `InitRenderResources(const RenderConfig&)`, `Render()`, and optional `UpdateFrameData(config, scene)`.
- `Create<T>(config, args...)` runs `make_unique` and then `InitRenderResources`.
- It holds `rhi_`, `vertex_shader_` and `pixel_shader_`.
- The header comment says one `PipelinePass` "may exist in one RHIRenderPass", but in practice **every pass calls `BeginRenderPass`/`EndRenderPass` itself** (for example `ForwardMeshPass.cpp:412-419`, `SkyBoxPass.cpp:73-78`, `ScreenQuadPass.cpp:145-152`, `UiPass.cpp:8-17`). No two passes share an `RHIRenderPass`.
- There is **no resize hook**, and no declaration of inputs or outputs.

Subclass families:

- `MeshPass` (`MeshPass.h`/`.cpp`) keeps one PSO per primitive, indexed by primitive index. It is built incrementally from `GetPrimitiveChangeList()` in `UpdateFrameData` (:9-44). `Render` loops primitives → `DrawMesh` (:46-53). Derived: `DepthPass`, `GBufferPass`, `ForwardMeshPass`.
- `ScreenQuadPass` (`ScreenQuadPass.h`) is a template method: `SetupRenderPass/Pipeline/Vertices/VS/PS/Compile/Bind*`. Derived: `ToneMappingPass`, `DirectionalLightingPass`.
- Standalone passes: `SkyBoxPass`, `UiPass`, `ClearTexturePass`, and the `IBLPass` family (compute; not driven by the renderer). `BlurPass` exists but has no users.

Wiring between passes:

- Resources are passed as `RHIResourceRef<RHIImage>` / `RHIResourceRef<RHIRenderTarget>` constructor arguments (shared_ptr, `RHI.h:265-284` deferred deleter), or as `PassResources` structs (`ForwardMeshPass.h:16-22`, `DirectionalLightingPass.h:18-27`).
- Each pass builds its **own** `RHIRenderTarget` and `RHIRenderPass` over the shared images. For example, `ForwardMeshPass` and `SkyBoxPass` both wrap `scene_color_+scene_depth_` in separate RT objects (`ForwardMeshPass.cpp:128-153`, `SkyBoxPass.cpp:104-117`).
- Load, store and layout choices are hard-coded per pass in `RHIRenderPass::Attribute`. The defaults are color load None / store Store, depth load None / store None (`RHIRenderPass.h:26-38`).
- Dynamic rewiring goes through ad-hoc setters that rebind descriptors: `SetInput`, `SetDirectionalShadow`, `SetIBL`, `SetSkyLight`, `OverrideSkyMap`. IBL readiness arrives through an `Event` subscription with a dirty flag (`ForwardMeshPass.cpp:382-399`).

Ownership of outputs is mixed:

- The pass owns its output in: `DepthPass` (`DepthPass.cpp:26-61`), IBL passes, NRD, MetalFX.
- The renderer owns the output in: `scene_color_`, the GBuffer, and the pool RTs.

Allocation:

- `RHIRenderTargetPool` (`RHIRenderTargetPool.h:11-17`, `.cpp:31-95`) reuses exact-attribute matches once the GPU is safe (after max-frames-in-flight frames, or after an idle flush). It exists mainly to survive **renderer recreation** (`tests/rhi/PipelineSwitchPoolTest.cpp`).
- It has only 5 call sites: `GPURenderer.cpp:103,122`, `ForwardRenderer.cpp:96`, `DeferredRenderer.cpp:50,85`.
- Renderers hold the targets for their whole lifetime. There is no per-frame acquire/release and no memory aliasing.
- Everything else comes straight from `rhi_->CreateImage`.

Resize and `render_scale`:

- A window resize only sets `frame_buffer_resized_` for swapchain recreation (`Renderer.cpp:77-80` → `RHI.h:83-86`). The present pass stretches the output to the surface and re-uploads pre-rotation when `IsBackBufferDirty()` (`ScreenQuadPass.cpp:154-167`).
- The internal `width`/`height` (default 1280×720) and `render_scale` (`RenderConfig.cpp:24-30`) form `RenderResolution`. Any change **recreates the whole renderer, and also the scene proxies** (`RenderFramework.cpp:280-311`). Every pass is rebuilt from scratch.

## 4. Shared resources: ownership and lifetime

- **GBuffer** (`resource/GBuffer.h`): a value member of `DeferredRenderer`. It holds a single `packed_texture` in `images[0]`. It is **copied** (its shared_ptrs) into `DirectionalLightingPass::PassResources` (`DeferredRenderer.cpp:96-103`). It lives as long as the renderer. `CPUGBuffer` is CPU-side vectors in `CPURenderer`.
- **View** (`resource/View.h` `ViewUBO`): the buffer is `CameraRenderProxy::view_buffer_`, uploaded in the camera's Update (`CameraRenderProxy.cpp:64`). Passes bind it at PSO creation (`GBufferPass.cpp:102`, `ForwardMeshPass.cpp` BindPassResources). Scene load can replace the camera proxy, so `DirectionalLightingPass` must detect the new camera by hand (`DirectionalLightingPass.cpp:78-83`). This is the stale-camera-proxy failure class.
- **PathTracingDenoiserInputs**: a `unique_ptr` in `GPURenderer` (`GPURenderer.cpp:125`). The trace writes it. Denoisers **borrow** it per `Encode` through the raw-pointer `DenoiserInputs` (`Denoiser.h:20-29`). It lives as long as the renderer and is reallocated when the `radiance_fp16` format changes.
- **DenoiserHandoff**: pure arithmetic, no resources (`DenoiserHandoff.h/.cpp`). It is the cross-fade schedule between denoiser output and accumulator (512 → 2048 spp) that both providers use.
- **ImageBasedLighting**: owned by `SkyRenderProxy` (`SkyRenderProxy.cpp:42-43`) and created only for raster pipelines. Renderers hold a raw pointer and re-point on sky-proxy change (`ForwardRenderer.cpp:378-396`). It owns 3 `IBLPass`es that own the cubemaps. It is persistent across frames and is cooked or loaded once per sky.
- **SSAOResource**: only an `SSAOConfig` struct (a random hemisphere kernel). It is embedded in UBOs but always value-initialized `{}` (`ForwardMeshPass.cpp:215`, `DirectionalLightingPass.cpp:95`). **No SSAO pass exists.** `use_ssao` only forces a prepass and a `TransferSrc` initial-layout assumption that nothing produces (`ForwardMeshPass.cpp:138-142`). It is vestigial.

## 5. Denoiser integration

- **Selection**: `GPURenderer` keeps `denoiser_slots_` in the order {MetalFx, Nrd} (:76-79). Each slot is created lazily through `CreateDenoiser` (`DenoiserFactory.cpp:10-37`) and latched as failed on error. `Auto` walks the order (`GPURenderer.cpp:577-646`). A selection change marks the camera dirty and resets history (:271-284).
- **Interface** (`Denoiser.h:47-59`): `NeedsInputs`, `UpdateFrameData(DenoiserFrameData)` (called in Update), `Encode(DenoiserInputs)` (called in Render; it records its own passes into the current command buffer), and `GetOutput`. Providers insert passes by **recording inline inside `Encode`**. The renderer cannot see them.
- **NRD** (`NrdDenoiser.cpp`):
  - `Initialize` (:166-312) allocates the NRD permanent and transient pools through `RHINrdBackend::AllocateResources` (backend-owned, invisible to the engine), the user textures `in_mv` RGBA16F, `in_normal_roughness` R10G10B10A2, `in_viewz` R32F, `in_diff`/`in_spec`/`out_diff`/`out_spec`/`validation` RGBA16F (all @ scene), and 3 timestamped compute passes (pack, reblur, resolve).
  - `Encode` (:363-462): pack → ReBLUR (`backend_->RunDispatches` inside one compute pass, :642-644) → barriers → resolve.
  - `run_reblur` is false after the handoff completes (:412), so a converged view runs only the resolve.
  - **Temporal state**: NRD's permanent pool (ReBLUR history), `output_history_` (read-modify-write EMA in place, not ping-pong: `nrd_resolve.cs.slang:41,120-122`), `prev_view/proj` matrices, `frame_index_`. `output_` and `output_history_` match the accumulator's format @ scene (:314-324). Upsampling happens later in tonemap.
- **MetalFX** (`libraries/source/rhi/metal/MetalFxDenoiser.mm`):
  - prepare compute (7 textures @ scene) → `MTLFXTemporalDenoisedScaler encodeToCommandBuffer:` **directly on the raw `MTLCommandBuffer`**, outside any RHI pass (:555-558) → optional resolve compute into `resolved_output` RGBA16F **@ output** (:202-213, 565-587).
  - Its history is internal to the scaler (`shouldResetHistory`, :545).
- **Timing**: `PassTimingAggregator` (`PassTimingAggregator.cpp:29-56`) harvests `RHIComputePass::GetExecutionTime(slot)` before recording which stages this slot will run, and logs means every 300 frames. Only compute passes created with `need_timestamp=true` are timed: the trace, the NRD stages, and the MetalFX prepare and resolve. Raster passes have **no GPU timestamps**. Frame GPU time comes from `RHIContext::GetFrameStats` (a Vulkan `RHITimer`, `VulkanRHI.cpp:56-65`; Metal command buffer `GPUStartTime`/`GPUEndTime`, `MetalRHI.mm:148-159`). The GPU renderer's dynamic spp reads the trace pass time (`GPURenderer.cpp:411-433`).

## 6. Debug and visualization facilities

These exist today:

- `RenderConfig::output_image` (SceneColor / IBLBrdfTexture / IBLDiffuseMap / IBLSpecularMap). It swaps in a `ScreenQuadPass` or overrides the skybox map (`ForwardRenderer.cpp:218-262`, `DeferredRenderer.cpp:210-249`).
- `RenderConfig::debug_mode`, a shader-level mode in `PbrConfig.mode` and the path-tracer camera UBO (`RenderConfig.h:33-47`).
- `NrdDebugMode`: 11 channel views rendered by `nrd_resolve` (`NrdConfig.h:9-23`).
- `Logger::LogToScreen` stat lines (SPP, accumulation, IBL progress, render-thread/GPU ms).
- The ImGui control panel (`AppFrameworkUi.cpp:94-153`). Its cvar editor is `ConfigManager::DrawUi` and it has a screenshot page (`RenderFramework::DrawUi`).
- The Tracy CPU zones `PROFILE_SCOPE` (`core/Profiler.h:9-28`).
- `[NrdPerf]`/`[MetalFxPerf]` log lines.
- The screenshot readback hook `Renderer::ReadbackFinalOutputIfRequested` (`Renderer.cpp:108-180`). It copies color image 0 of any RT to a staging buffer, with an end-of-frame PNG write.
- Right-click debug point (used by the CPU renderer only).

This does not exist: a generic "view any intermediate texture" facility or per-raster-pass GPU timing. A graph visualizer could plug in at three points:

1. A texture-view overlay. `ScreenQuadPass(input=any image, target=screen color)` is already the mechanism behind `output_image`.
2. The readback hook for dumping any graph resource.
3. `PassTimingAggregator`, extended to raster passes with timestamp support on `RHIRenderPass`.

## 7. Persistent and transient resources, memoryless candidates, and mergeable passes

**Persistent (must survive frames):**

- the GPU accumulator `scene_texture_`
- the denoiser outputs (a frozen frame redisplays them)
- NRD's permanent pool and `output_history_`
- MetalFX scaler history
- IBL cubemaps and the BRDF LUT
- TLAS/BLAS and bindless material/texture buffers
- the camera view UBO
- the CPU `frame_buffer_`

**Per-frame transient in principle:**

- shadow map, prepass/scene depth, GBuffer, `scene_color_` / `lighting_rt_`
- screen color. Caveat: screenshot readback copies it within the frame, which needs store.
- `PathTracingDenoiserInputs`, but only if the resolve and the frozen-frame display did not read them later. The NRD resolve reads them the same frame, so they are transient in frame-lifetime terms.
- NRD `in_*`, and MetalFX prepared textures.
- NRD's transient pool. Its 8 lifetimes overlap, so aliasing does not help (`docs/Denoiser.md`, NRD Performance section).

**Memoryless (tile-only) candidates on TBDR.** RHI support for this is missing today: `ImageUsage::TransientAttachment` exists (`RHIImage.h:130`), but `GetVkImageUsage` does not map it to `TRANSIENT_ATTACHMENT`/`LAZILY_ALLOCATED` (`VulkanImage.h:229-257`), and Metal only maps it to RenderTarget usage, with storage mode taken from `memory_properties` (`MetalImage.mm:14-22,149`).

- Deferred GBuffer packed RGBAUInt32. It is read only at the same pixel (`directional_lighting.ps.slang:46` `Load`).
- Deferred and Forward scene depth. The lighting pass reads it at the same pixel (nearest `Sample` at uv, :50), and the skybox reads it only as a depth test.
- `scene_color_` / `lighting_rt_`, **only when `render_scale == 1`**. With upsampling, tonemap samples it bilinearly at other pixels.
- Screen color, only if tonemap+UI rendered straight into the swapchain image, the readback was restructured, and output == surface size. Not possible today, because present stretches and pre-rotates.

**Mergeable raster chains** (today every pass is its own render pass; `NextSubpass` is `UnImplemented` on both backends, `VulkanRHI.h:65-68`, `MetalRHI.mm:182-185`; Vulkan render passes always have `subpassCount = 1`, `VulkanRenderPass.cpp:121`):

- **Forward base + skybox**: same color and depth images, consecutive draws, no subpass needed. Just one render pass. The code already marks this: `// TODO(tqjxlm): avoid the additional pass here` at `SkyBoxPass.cpp:108`.
- **Deferred GBuffer → lighting → skybox**: one render pass with 2 subpasses. Subpass 1 writes gbuffer+depth. Subpass 2 takes the gbuffer as an input attachment (framebuffer fetch on Metal) and depth as a read-only depth attachment plus input, writes scene color, and then draws the skybox in the same subpass with a read-only depth test. This removes the depth Read↔DepthStencilOutput ping-pong (`DeferredRenderer.cpp:143-152`).
- **Tonemap + UI**: same target, tonemap load None then UI load Load, so one render pass. The mid-chain "readback without UI" copy (`*Renderer.cpp`, the `has_readback` blocks) currently splits them. A graph would have to make that copy a conditional pass that forces a split, or do the readback from a separate resolve.
- **Tonemap as a subpass of the lighting pass**: only valid when `scene == output` and the debug quad is inactive.
- **Present**: must stay separate while it stretches or rotates, unless tonemap writes the back buffer directly with pre-rotation in the VS.
- **GPU clear**: `ClearTexturePass` is a raster clear of a UAV accumulator (`GPURenderer.cpp:131-132,156-160`). A graph could turn it into a clear command or fold it into the trace dispatch.

## 8. Multithreaded command recording

There is none. All GPU recording is serial on the render thread into a single command buffer:

- There are no secondary command buffers or `MTLParallelRenderCommandEncoder`. A search for `SECONDARY`, `vkCmdExecuteCommands` and `parallelRenderCommandEncoder` in `libraries/source/rhi` finds nothing.
- The RHI keeps global mutable recording state: `current_render_pass_`, `current_compute_pass_`, the context's current command buffer used by `VulkanImage::Transition` (`VulkanImage.cpp:320-323`), and per-image layout tracking.
- Resource creation asserts the render thread (for example `RHIRenderTargetPool.cpp:34`).
- Metal `Transition` is a no-op (`MetalImage.h:33`), relying on automatic hazard tracking.
- Worker threads are used only for CPU path tracing (`CPURenderer.cpp:437-449`), cook jobs, and BVH building.

## Implications for a render graph

### Seams worth keeping

- `Renderer::Tick` (Update) vs `Render` is already a rough setup/execute split. UBO uploads and change-list processing happen before recording. A graph "build/compile" step fits at the end of `Update`.
- `RenderResolution` is the single source for the scene/output extents. Graph resources can be declared against "scene" or "output" size classes.
- `RHIRenderTargetPool` is the natural backing allocator. It already handles GPU-safe reuse across frames and renderer recreation. It needs to go from exact-match and renderer-lifetime holding to per-frame acquire/release with lifetime-based reuse. Aliasing is later work.
- `Denoiser` is already a provider-neutral boundary. `DenoiserInputs`/`GetOutput` map directly to declared graph inputs and outputs of an opaque node.
- `PassTimingAggregator` and `ReadbackFinalOutputIfRequested` are ready hooks for per-node timing and "dump or view any resource".

### Pain points the graph must absorb

1. **Barriers are hand-written** at renderer level, with hand-picked stages, and duplicated across all four renderers (`DeferredRenderer.cpp:136-161`, `GPURenderer.cpp:170-190`, the readback transitions in every renderer). Vulkan relies on stateful per-image layout tracking. Derive these from declared read and write usage instead.
2. **Wiring is implicit.** Resources flow through constructors and setters with descriptor rebinding (`SetInput`, `SetIBL`, `OverrideSkyMap`, camera-proxy change detection). A declarative per-frame read/write list per pass would remove this whole class, including the stale-camera bugs.
3. **Passes own their render passes and RTs**, and load/store ops are baked in at construction. The graph must own `RHIRenderPass` creation (load/store from first and last use, merging, subpasses). Passes should only record draws into a render pass the graph provides.
4. **No resize**: any resolution change rebuilds renderer + scene proxies (`RenderFramework.cpp:278-321`). With graph-owned transient resources, a resolution change could reallocate only the resources and passes that depend on it.
5. **Conditional topology is imperative**: NeedClear, will_dispatch, gbuffer_write, run_reblur/run_resolve, output_image, sky and directional-light presence, render_ui, readback with or without UI. The graph should either be rebuilt per frame (cheap, since there are few nodes) or support per-node enable predicates.
6. **GPU work hidden outside Render**: IBL cooking dispatches in `SceneRenderProxy::Update`, TLAS builds in `Update`, IBL `Finalize` in an end-of-frame task with its own command buffer and a synchronous readback, the Vulkan BLAS in one-shot command buffers. These should become explicit graph nodes, or at least declared external producers of IBL, TLAS and bindless resources, so dependencies and timing are visible.
7. **Third-party nodes**: NRD's `RunDispatches` has backend-internal pools. MetalFX encodes directly on the `MTLCommandBuffer` with no active encoder. ImGui calls `ui_handler_->BeginFrame()` inside Render. These need an "opaque/external pass" node type that declares inputs, outputs and the required encoder state, with its internal resources left out of graph aliasing.
8. **Persistent and history resources** (accumulator, NRD permanent pool and `output_history_`, denoiser output redisplayed on frozen frames, IBL maps, TLAS) need first-class "imported/persistent" graph resources whose lifetime is not bounded by the frame.
9. **Latent issues found while mapping** (static reading, not run):
   - `pre_pass_->UpdateFrameData` is never called (`ForwardRenderer.cpp:287-290`), so its `MeshPass` PSO table stays empty while `MeshPass::Render` indexes it (`MeshPass.cpp:48-52`). The prepass path (default off) appears broken.
   - `SkyBoxPass` is initialized twice, via `Create` plus an explicit `InitRenderResources` (`ForwardRenderer.cpp:402-404`, `DeferredRenderer.cpp:347-349`).
   - `DirectionalLightingPass::SetIBL(nullptr)` dereferences `ibl` when a bound sky is removed (`DirectionalLightingPass.cpp:183-199`). Forward guards against this (`ForwardMeshPass.cpp:391`).
   - SSAO is vestigial. `BlurPass` and Forward's ray-tracing branch are dead code.
   - `docs/Denoiser.md` refers to `RHIDenoiser` / `rhi/RHIDenoiser.h`, but the interface is `renderer/denoiser/Denoiser.h`.

**Candidate merges** (see section 7): Forward base+skybox, then Deferred GBuffer→lighting(+skybox) as a subpass render pass, then tonemap+UI. Each one first needs RHI work: multi-subpass `RHIRenderPass` with input attachments / framebuffer fetch, read-only depth layouts, and a real `NextSubpass`. The best TBDR win is the Deferred GBuffer + depth becoming memoryless at `render_scale == 1`. That also needs `TransientAttachment` mapped to `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` + `LAZILY_ALLOCATED` memory and to `MTLStorageModeMemoryless`.

### Candidate parallel recording

- The shadow `DepthPass` and the GBuffer/base pass are independent mesh loops with per-primitive PSOs already built, so they are natural parallel-recording units.
- Large `MeshPass` loops could also be split by primitive range.
- The IBL cook compute is independent of the raster chain.
- None of this is possible until the RHI gets per-thread command contexts: secondary command buffers or multiple primary buffers on Vulkan; parallel render encoders or multiple command buffers on Metal. Recording state (`current_render_pass_`, current command buffer, image layout tracking, dynamic UBO suballocation) would have to move off the global `RHIContext`, and layout transitions would have to be resolved by the graph up front rather than recorded ad hoc.

### Changes the pass API needs

- Replace `InitRenderResources/Render/UpdateFrameData` with:
  - (a) persistent setup (PSOs, shaders) that is independent of resource identity;
  - (b) a per-frame `Setup(builder)` that declares reads and writes (texture, attachment with load/store intent, storage, depth read-only), size class (scene/output/fixed), format and persistence;
  - (c) `Execute(context, resolved_resources)`, which records into a graph-provided render or compute pass and binds resolved resources per frame rather than at construction.
- Add pass kinds: raster, compute, copy, AS build, external/opaque (NRD, MetalFX, imgui).
- Add a per-node timestamp query (raster included) and a debug name, so the aggregator and a visualizer can enumerate nodes and resources.
