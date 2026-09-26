# Sparkle RHI: how it works today, and what that means for a render graph

Scope: `libraries/include/rhi/*`, `libraries/source/rhi/{*.cpp,vulkan/*,metal/*}`. I read these read-only and changed nothing. Paths below are relative to `libraries/`, with `inc/` = `include/rhi/`, `src/` = `source/rhi/`, `vk/` = `source/rhi/vulkan/`, `mtl/` = `source/rhi/metal/`. I also looked at a few renderer and framework files to confirm call patterns.

## 1. Object model

**RHIContext** (`inc/RHI.h:29-399`) is the abstract device and frame object. `CreateRHI` picks `VulkanRHI` or `MetalRHI` from the config (`src/RHI.cpp:21-43`). There is no RHIContext singleton: the renderer holds a pointer to it. Each backend does have a global singleton for its native state:

- Vulkan: `inline std::unique_ptr<VulkanContext> context` (`vk/VulkanContext.h:378-380`, marked "TODO avoid singleton").
- Metal: `inline std::unique_ptr<MetalContext> context` (`mtl/MetalContext.h:108-110`).

Every backend object reaches the device, the current command buffer and the RHIContext through this global.

**RHIResource** (`inc/RHIResource.h:22-87`) holds:

- a name;
- `IsDynamic` and `IsBindless` flags;
- a process-unique `GetId()`, taken from an atomic counter (`src/RHIResource.cpp:11-17`). The id is regenerated whenever `id_dirty_` is set, for example when a TLAS gets a new BLAS (`inc/RHIRayTracing.h:61-77`) or a render target is recreated (`vk/VulkanRenderTarget.cpp:43`).

The id is what descriptor caching keys on (`src/RHIShader.cpp:46-65`), so it is a ready-made stable handle key.

**Creation and lifetime.** All resources come from backend factory methods (`CreateImage`, `CreateBuffer` and so on, `inc/RHI.h:161-217`). These go through `RHIContext::CreateResource<T>` (`inc/RHI.h:264-284`), which:

- wraps the object in a `std::shared_ptr` with a deleter that calls `DeferResourceDeletion`;
- in debug builds, also registers a weak ref and the allocation stack for leak checking (`src/RHI.cpp:184-206`).

`RHIResourceRef<T>` is a thin `shared_ptr` wrapper with optional ref-count tracing (`inc/RHIResource.h:99-275`).

Deletion is deferred per frame slot:

- `deferred_deletion_[frame_index_]` is a ring of size max-frames-in-flight (`inc/RHI.h:382-383`), guarded by a mutex so a release from another thread is safe (`src/RHI.cpp:335-388`).
- The slot is drained in `BeginFrame` only after `BeginFrameInternal` has waited that slot's fence (`src/RHI.cpp:220-235`).
- `EnqueueEndOfRenderTasks` (`inc/RHI.h:290-293`) is the GPU-completion callback substitute. It is used, for example, to destroy old TLAS handles (`vk/VulkanRayTracing.cpp:219-221`).
- `FlushDeferredDeletions` requires a prior device idle (`src/RHI.cpp:428-449`).

**Resources are not cached by name.** Only these are cached:

- samplers, by attribute (`src/RHI.cpp:89-101`);
- dummy textures, by shader-compatibility hash (`src/RHI.cpp:468-492`);
- image views, per image by view attribute (`src/RHIImage.cpp:59-72`); views are owned by the image (`inc/RHIImageView.h:10-12`), and the Vulkan image destructor kills view handles early (`vk/VulkanImage.cpp:361-373`);
- dynamic sub-allocations, by (usage, capacity) (`src/RHIBuffer.cpp:142-173`).

`RecreateBuffer` grows a buffer by powers of two (`src/RHI.cpp:281-301`).

**Per-frame memory:**

- `AllocateOneFrameMemory<T>` is a stack allocator reset in `BeginFrame` (`inc/RHI.h:295-300`, `src/RHI.cpp:211`). Vulkan uses it for `VkDescriptor*Info` payloads.
- Dynamic buffers (`RHIDynamicBuffer`) are persistently mapped rings of capacity × frames-in-flight. The offset is `offset + frame_stride*frame` (`inc/RHIBuffer.h:34-38`, `src/RHIBuffer.cpp:83-140`), and `Upload` memcpys into the current slot (`src/RHIBuffer.cpp:242-249`).
- Staging buffers come from the dynamic pool when a frame is active (`src/RHI.cpp:303-314`).

**Frames in flight** are fixed on first set, then frozen (`src/RHI.cpp:408-426`):

- Vulkan: the swap-chain image count, `minImageCount+1` (`vk/VulkanSwapChain.cpp:130,197`); 2 in headless mode (`vk/VulkanRHI.cpp:27,302`).
- Metal: 3 (`frameworks/source/apple/MetalView.mm:125`); 2 in headless mode (`mtl/MetalContext.mm:9,55`).

`frame_index_` advances in `EndFrame` (`src/RHI.cpp:255`).

**RHIRenderTargetPool** (`inc/RHIRenderTargetPool.h`, `src/RHIRenderTargetPool.cpp`):

- It matches requests exactly on `RHIRenderTarget::Attribute` and creates the images itself on a miss (lines 31-76).
- A target is "free" when `use_count()==1` for the target and all of its images (lines 12-29).
- A free target becomes reusable ("gpu_safe") `maxFramesInFlight` frames after its last use (lines 78-95), or immediately after a device idle (lines 116-125).
- It never releases on its own; `ReleaseUnused` is manual. It asserts it runs on the render thread (lines 34, 99).
- The renderers acquire at init time (`renderer/DeferredRenderer.cpp:50,85`, `GPURenderer.cpp:103,122`, `ForwardRenderer.cpp:96`). It is therefore a cache that survives renderer recreation, not a per-frame transient allocator. It does no aliasing and no memory sharing.

**RHIResourceArray (bindless)** (`inc/RHIResourceArray.h`):

- It is a fixed-capacity vector of `RHIResourceRef<RHIResource>` with dirty indices (`src/RHIResourceArray.cpp:9-21`). It counts as bindless when capacity is 1024 (`inc/RHIShader.h:79`, `inc/RHIResourceArray.h:37-40`).
- **Vulkan** uses `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND` descriptors, enabled only when ray tracing is supported (`vk/VulkanDescriptorSetManager.cpp:49-63,346-355`; `vk/VulkanContext.cpp:964-969`).
  - Dirty arrays are flushed only in `VulkanRenderPass::Begin` (`vk/VulkanRenderPass.cpp:156`), not in compute-pass begin (`vk/VulkanComputePass.cpp:22-34`).
- **Metal** uses an argument buffer patched in place at `Bind`, plus `useResources:usage:Read` (`mtl/MetalResourceArray.mm:83-125`).

**RHIMemory** (`inc/RHIMemory.h`) holds five flags: HostVisible, HostCoherent, HostCached, DeviceLocal, AlwaysMap.

- On Vulkan they map to VMA required flags (`vk/VulkanBuffer.cpp:36-63`, `vk/VulkanImage.cpp:35-40`).
- On Metal, DeviceLocal → Private and everything else → Shared (`mtl/MetalBuffer.h:47-56`, `mtl/MetalBuffer.mm:10-24`).

## 2. Images and buffers: attributes, state tracking, barriers

**RHIImage::Attribute** (`inc/RHIImage.h:139-166`) holds:

- format, sampler attribute, width, height;
- usages: TransferDst, TransferSrc, Texture, SRV, UAV, ColorAttachment, DepthStencilAttachment, TransientAttachment (`inc/RHIImage.h:120-131`);
- memory properties, mips, msaa, `initial_layout`;
- type: 2D or Cube only. There are no 2D arrays or 3D images; a cube is 6 layers (`inc/RHIImage.h:258-270`).

**No transient or memoryless support exists.** `TransientAttachment` is dropped by `GetVkImageUsage` (`vk/VulkanImage.h:229-257`), so there is no `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` and no `LAZILY_ALLOCATED` memory. On Metal it only adds `MTLTextureUsageRenderTarget` (`mtl/MetalImage.mm:19-23`); `MTLStorageModeMemoryless` is never used.

**RHIBuffer::Attribute** (`inc/RHIBuffer.h:75-82`) holds size, usages, memory properties and `is_dynamic`. Usages are TransferSrc/Dst, Uniform, Vertex, Index, Storage, DeviceAddress, AS build input, AS storage. **Buffers have no state tracking and no barrier API at all.** The only buffer barrier in the RHI is a blanket `TRANSFER→ALL_COMMANDS` after `CopyToBuffer` (`vk/VulkanBuffer.cpp:93-104`). A compute shader writing a storage buffer that is later read as vertex, index or indirect data has no RHI-level synchronization.

**Image layout tracking (the real resource-state tracker):**

- `RHIImage` stores one `RHIImageLayout` per (mip, layer) in `current_layout_` (`inc/RHIImage.h:296-318,330`), initialized from `initial_layout` (`src/RHIImage.cpp:51`).
- `RHIImageLayout` values are Undefined, General, Read, StorageWrite, ColorOutput, DepthStencilOutput, TransferSrc, TransferDst, PreInitialized, Present (`inc/RHIImage.h:17-29`).
- `RHIPipelineStage` is a coarse single-value enum: Top, DrawIndirect, VertexInput, VS, PS, EarlyZ, LateZ, ColorOutput, CS, Transfer, Bottom (`inc/RHIImage.h:31-44`). It has no ray-tracing, AS-build or host stage.

**Barriers are explicit and caller-driven.** `RHIImage::Transition(TransitionRequest{target_layout, after_stage, before_stage, mip/layer range})` (`inc/RHIImage.h:168-178,200`) maps on Vulkan to `TransitionLayout` on the current command buffer (`vk/VulkanImage.cpp:54-127,320-323`):

- It groups contiguous mips that share the same old layout into one `VkImageMemoryBarrier` each (lines 98-118).
- **It skips the barrier entirely when the old and new VkImageLayout are equal** (lines 77-80). So StorageWrite→StorageWrite, and General↔StorageWrite (both `GENERAL`, `vk/VulkanImage.h:265-272`), emit no barrier. Write-after-write or read-after-write on a storage image in a constant layout is left unsynchronized.
- Access masks are derived from (layout, caller-given stage) only (`vk/VulkanImage.h:9-66`). The tracker records no previous access or stage.
- `General` combined with a non-Top/Bottom stage hits `ASSERT_F(false, …)` (`vk/VulkanImage.h:60-63`).
- It then writes the new layout (line 126).

The renderer issues these transitions by hand around passes, for example:

- `renderer/GPURenderer.cpp:170-190` (StorageWrite before compute, Read after);
- `DeferredRenderer.cpp:119-201`;
- `BlurPass.cpp:56-58,147-152`.

The only transitions that are *not* explicit:

- **Render-pass implicit layouts.** The VkRenderPass uses the attribute's `color_initial_layout` / `final_layout` (`vk/VulkanRenderPass.cpp:24-96`). `End()` then overwrites the tracked layout with `final_layout` (`vk/VulkanRenderPass.cpp:225-241`). `Begin()` never checks the tracked layout against `color_initial_layout`, so the tracker and the pass attribute are two unreconciled sources of truth.
- The **Present** transition, inserted in `VulkanContext::EndFrame` (`vk/VulkanContext.cpp:538-540`).
- Upload helpers (`vk/VulkanImage.cpp:144-150,177-183`), `GenerateMips` (lines 217-234) and readback (`src/RHIImage.cpp:20-26`).
- Dummy textures rest permanently in Read or General (`src/RHI.cpp:483-488`).
- Descriptor writes **bake** the expected layout: `SHADER_READ_ONLY_OPTIMAL` for sampled images, `GENERAL` for storage (`vk/VulkanImage.cpp:446-449`). Callers must make the tracked layout match at use time; nothing checks this at bind.

**`VulkanSynchronization.cpp`** is only `VulkanFence::Wait()` (a spin on `vkGetFenceStatus` plus `vkWaitForFences`, lines 9-17). It is used to block the next frame on a one-shot command buffer (`vk/VulkanCommandBuffer.cpp:61-65`). There is no sync2, no timeline semaphores and no events; the API version is Vulkan 1.1 (`vk/VulkanCommon.h:28`).

**`RHITrackedState.h` is not resource-state tracking.** It is a redundant-*command* filter:

- `Update(value)` bit-casts a trivially-copyable key and returns true only when it differs from the last recorded value (`inc/RHITrackedState.h:14-41`). Comparison is bit-exact by design, so NaN payloads and -0 count as distinct.
- `VulkanContext::CommandState` holds `RHITrackedState` instances for: graphics and compute pipeline, viewport+scissor, vertex buffers (≤8), index buffer, and descriptor sets (≤8 per bind point, including dynamic offsets) (`vk/VulkanContext.h:307-367`).
- All binds go through `BindPipeline`, `SetViewportAndScissor`, `BindVertexBuffers`, `BindIndexBuffer` and `BindDescriptorSet` (lines 122-213). The multi-set path `BindDescriptorSets` records unconditionally and invalidates slots (lines 215-227); NRD uses it.
- `ResetCommandState` is called only in `BeginFrame` (`vk/VulkanContext.cpp:436,504`). It is *not* called in `BeginCommandBuffer` for one-shot buffers (lines 695-710), and *not* after foreign code records raw `vkCmd*`. ImGui does exactly that (`vk/VulkanUi.cpp:93`), and the header comment warns this makes the filter stale (`vk/VulkanContext.h:122-123`). Today a PresentPass `DrawMesh` follows the UI pass (`renderer/DeferredRenderer.cpp:188,206`). This is benign as long as the next pass's pipeline, vertex buffer and viewport differ from the pre-UI values, but it is order-sensitive.

**Metal** has no layout model: `MetalImage::Transition` is an empty function (`mtl/MetalImage.h:33-35`). Hazard tracking is the default, meaning *automatic*: `hazardTrackingMode`, `MTLFence` and `MTLEvent` appear nowhere in `src/rhi`. Compute encoders use `MTLDispatchTypeSerial` (`mtl/MetalComputePass.mm:15`), so dispatches within a pass are serialized. Indirectly referenced resources, meaning argument-buffer textures and TLAS→BLAS, are declared via `useResource(s):usage:Read` (`mtl/MetalResourceArray.mm:103-119`, `mtl/MetalRayTracing.mm:324-334`).

## 3. Render passes

- **`RHIRenderTarget`** (`inc/RHIRenderTarget.h`) holds:
  - up to 8 color images plus 1 depth image;
  - one `mip_level` and one `array_layer` for *all* attachments;
  - msaa, `need_clear_`, and an `is_back_buffer_` flag (the back-buffer constructor, `src/RHIRenderTarget.cpp:7-19`).
- **`RHIRenderPass::Attribute`** (`inc/RHIRenderPass.h:26-38`) holds:
  - load op (None / Load / Clear) and store op (None / Store), shared by all color attachments;
  - color initial and final layouts, and a clear color;
  - depth load, store, initial and final settings.
  - The pass holds a *weak* ref to the RT (line 57).
- **Vulkan: classic `VkRenderPass` + `VkFramebuffer`, no dynamic rendering** (API 1.1).
  - There is one subpass (`vk/VulkanRenderPass.cpp:99-122`), with one external→0 dependency: srcAccess=0, color/early-Z stages (lines 124-134).
  - `NextSubpass` is `UnImplemented` (`inc/VulkanRHI.h:65-68`; Metal `mtl/MetalRHI.mm:182-185`).
  - Framebuffers are created at pass creation: one per swap-chain image for the back buffer, otherwise one (`vk/VulkanRenderPass.cpp:190-214,258-274`). The pass is therefore bound to the concrete image views.
  - Depth is always cleared to 1.0 (line 181).
  - MSAA resolve images are hidden inside `VulkanRenderTarget::CreateMsaaResources` (`vk/VulkanRenderTarget.cpp:116-138`).
  - A static list of every created pass is kept (`vk/VulkanRHI.cpp:29,400-402`; it is never pruned) so that back-buffer passes can be re-initialized on swap-chain recreation (lines 250-264).
- **Graphics PSOs are baked against the pass.**
  - Vulkan: `pipeline_info.renderPass` (`vk/VulkanPipelineState.cpp:192`), with viewport and scissor taken from the RT extent at compile time (lines 306-323).
  - Metal: attachment pixel formats come from `render_pass_`'s RT (`mtl/MetalPipelineState.mm:377-405`).
- **Metal:**
  - One `MTLRenderPassDescriptor` per pass object, but it is refilled from the RT's *current* images on every `Begin` (`mtl/MetalRenderPass.mm:47-112`), which is more flexible than Vulkan.
  - One new render encoder per pass, labeled with the pass name (lines 54-58), with a flipped viewport (lines 60-66).
  - Discrepancies with Vulkan:
    - The clear color is hard-coded to (0,0,0,1) and ignores `attribute_.clear_color` (line 94).
    - It ignores `array_layer`: only `.level` is set (lines 92, 101).
  - No memoryless storage, no tile shaders and no imageblocks.

## 4. Compute passes, PSOs, binding model, declared I/O

- **`RHIComputePass`** (`inc/RHIComputePass.h`) holds only a name, a `need_timestamp` flag and per-frame execution times.
  - `Begin/EndComputePass` asserts that passes are exclusive and unnested (`src/RHI.cpp:271-333`).
  - On Vulkan, `Begin` and `End` only drive timers: no barrier, no label (`vk/VulkanComputePass.cpp:22-43`).
  - On Metal, `Begin` creates a serial compute encoder (`mtl/MetalComputePass.mm:26-55`).
  - `DrawMesh` and `DispatchCompute` pull the encoder or command buffer from the implicit `current_render_pass_` / `current_compute_pass_` (`mtl/MetalRHI.mm:187-223`, `vk/VulkanRHI.cpp:424-462`).
- **`RHIPipelineState`** (`inc/RHIPIpelineState.h`) holds Graphics or Compute shaders per stage, depth, raster and blend state, vertex and index buffers, and a render pass. `SetShader` instantiates the shader's `ResourceTable` (lines 116-131), and `GetShaderResource<T>()` returns the typed table (lines 163-168).
- **Binding model:**
  - `USE_SHADER_RESOURCE(name, type)` / `_BINDLESS` (`inc/RHIShader.h:401-412`) declares a typed `RHIShaderResourceBindingTyped` member. The member self-registers into the table (`src/RHIShader.cpp:104-110`).
  - Reflection assigns (set, slot):
    - Vulkan: spirv-reflect by name (`vk/VulkanShader.cpp:14-36,53-66`).
    - Metal: MTLBinding by name, all in set 0, with a SPIR-V alias fallback (`mtl/MetalPipelineState.mm:180-237`).
  - `BindResource(ptr)` type-checks at compile time (`inc/RHIShader.h:156-217`) and marks the set dirty (`src/RHIShader.cpp:112-133`). It stores a raw pointer and holds no reference.
  - At draw or dispatch time, Vulkan resolves a descriptor set from a cache keyed by (layout hash, hash of bound resource ids). The cache is shared across PSOs (`vk/VulkanDescriptorSetManager.cpp:204-249`, `vk/VulkanDescriptorSet.cpp:11-76`).
  - Freed sets return to the free list at end of the *current* frame (`vk/VulkanDescriptorSetManager.cpp:251-268`) and can be re-`vkUpdateDescriptorSets`'d next frame. **This looks like a reuse-while-in-flight risk worth verifying.**
- **Declared read/write info you can harvest today:**
  - Per PSO and stage, `RHIShaderResourceTable::GetBindings()` gives the resource type and the bound `RHIResource*`.
    - `Texture2D` = sampled read; `StorageImage2D` = RW; `UniformBuffer` / `DynamicUniformBuffer` = read; `AccelerationStructure` = read.
    - `StorageBuffer` is ambiguous. So is `StorageImage2D`, which may be read-only.
    - An `RHIImageView` gives image + mip/layer range (`inc/RHIImageView.h:23-44`).
  - Graphics: RT colors and depth, plus load/store, from `RHIRenderPass → RHIRenderTarget`; vertex and index buffers from the PSO.
  - Precise R/W access is available but not captured today:
    - SPIR-V `NonWritable` / `NonReadable` decorations (spirv-reflect `decoration_flags`);
    - `MTLBinding.access` on Metal.
  - Missing: which PSOs a pass will use, and when bindings change. Bindings are mutable PSO state set by user code at any time before record.

## 5. Command buffers, submission, frames, timers, threading

- **Vulkan:**
  - One primary command buffer per frame in flight, from a single graphics-family pool with RESET_COMMAND_BUFFER (`vk/VulkanContext.cpp:591-603,990-1000`).
  - One fence per frame, plus per-swap-chain-image fence aliasing (lines 447-496, 605-614).
  - Acquire semaphores: 2× the image count, round-robin. Finish semaphores: one per image (lines 621-646).
  - `BeginFrame` waits the fence, acquires (recreating on out-of-date), then begins the command buffer (lines 421-508).
  - `EndFrame` transitions to Present, submits with the acquire wait at `COLOR_ATTACHMENT_OUTPUT` (lines 549-553), presents, then pops finished one-shot buffers (lines 510-589).
  - **A single universal queue** handles graphics, compute and transfer ("all … work uses one universal queue", `vk/VulkanContext.h:272`). Only one queue is created (`vk/VulkanContext.cpp:896-988`). There is no async compute and no transfer queue.
  - Out-of-frame work uses `BeginCommandBuffer` / `SubmitCommandBuffer`, which make a temporary `OneShotCommandBufferScope` (lines 695-724). This submits to the same queue with its own fence and no semaphore. `should_block_next_frame` enqueues a fence wait before the next frame (`vk/VulkanCommandBuffer.cpp:38-69`).
  - BLAS builds each submit their own one-shot command buffer (`vk/VulkanRayTracing.cpp:80-85`).
- **Metal:**
  - One `MTLCommandBuffer` per frame (`mtl/MetalContext.mm:67-112,177-188`).
  - Throttling:
    - windowed: MetalView's in-flight semaphore, waited in `MetalView.mm:97` and signaled in the completion handler (`mtl/MetalContext.mm:101-103`);
    - headless: a dispatch semaphore (lines 70-73, 92-95).
  - Encoders:
    - render: one per pass;
    - compute: one per pass;
    - blit: created ad hoc per copy, mip generation or readback, on the current command buffer (`mtl/MetalImage.mm:297-356`, `mtl/MetalBuffer.mm:26-57`). This is illegal while another encoder is open, so copies must stay outside passes.
  - Private-texture uploads use a standalone command buffer plus `waitUntilCompleted` (`mtl/MetalImage.mm:181-218`).
  - **A TLAS build commits the frame's command buffer mid-frame, blocks, and begins a new one** (`mtl/MetalRayTracing.mm:203-222`).
- **Where BeginFrame, EndFrame and Present happen:**
  - The render thread calls `RenderFramework::RenderLoop` → `BeginFrame` → `renderer_->Tick/Render` → `EndFrame` (`application/RenderFramework.cpp:138-163,323-379`), which asserts `IsInRenderThread`.
  - `RHIContext::BeginFrame` runs before-frame tasks, `BeginFrameInternal`, then drains deferred deletion and end-of-render tasks for this slot (`src/RHI.cpp:208-237`).
  - `RHIContext::EndFrame` runs `EndFrameInternal` (submit and present), end-of-frame tasks, advances the index and ticks the RT pool (lines 239-260).
  - The IBL cook drives its own BeginFrame/EndFrame (`renderer/resource/IblCookAccelerator.cpp:58-63`).
- **Timers:**
  - `RHITimer` has Begin/End/TryGetResult with a status machine (`inc/RHITimer.h`). Compute passes with `need_timestamp` keep one timer per frame in flight and read the previous result at `Begin` (`vk/VulkanComputePass.cpp:10-43`, `mtl/MetalComputePass.mm:11-55`).
  - Vulkan: a 2-query pool, with both timestamps at `BOTTOM_OF_PIPE` (`vk/VulkanTimer.cpp:31-73`).
  - Metal: a counter sample buffer attached at encoder start and end, resolved in a completion handler (`mtl/MetalTimer.mm:40-94`). Only compute passes can be timed on Metal.
  - **Render passes have no GPU timing on either backend.**
  - Frame GPU time:
    - Vulkan: a frame timer (`vk/VulkanRHI.cpp:56-75`);
    - Metal: `GPUStartTime/EndTime` of the last command buffer (`mtl/MetalRHI.mm:150-156`). This undercounts when a TLAS build has split the frame.
- **Threading.** Recording is single-threaded on the render thread: one command pool, the global `context`, unsynchronized task vectors (`inc/RHI.h:254-262`), and pool asserts (`src/RHIRenderTargetPool.cpp:34`). There is no RHI thread. Only deferred deletion is mutex-protected.

## 6. Foreign passes

- **Ray tracing:**
  - Vulkan BLAS: a separate submit (`vk/VulkanRayTracing.cpp:80-85`).
  - Vulkan TLAS: `vkCmdBuildAccelerationStructuresKHR` into the current frame command buffer (line 257). `Update` writes the host-visible instance buffer via `Lock` (lines 151-170).
  - **No AS-build→shader-read barrier, and no BLAS→TLAS cross-submit dependency, appears anywhere in the RHI.** `RHIPipelineStage` cannot express one either.
  - Metal: an AS encoder on the frame command buffer, with the blocking mid-frame commit described in §5.
- **NRD** (`inc/RHINrdBackend.h`) must be bracketed by a caller's `Begin/EndComputePass` (lines 80-83).
  - Vulkan (`vk/VulkanNrdBackend.cpp:363-495`):
    - private pool images are raw `VkImage`s kept in GENERAL and invisible to the RHI tracker (`vk/VulkanNrdBackend.h:54-67`, cpp 331-361);
    - a full compute→compute memory barrier between dispatches (lines 381-389);
    - user images go through `RHIImage::Transition`, so they stay tracked (lines 464-472);
    - a per-frame descriptor pool reset (lines 373-374);
    - a 256-slot host-written constant ring (lines 300-309, 418-426);
    - binds via `context->BindPipeline` / `BindDescriptorSets`, so it is tracked-state-safe (lines 490-492).
  - Metal: records into the current compute encoder and relies on serial dispatch plus auto-tracking; its pool textures are raw `MTLTexture`s (`mtl/MetalNrdBackend.mm:180-241`, `.h:47-48`).
- **MetalFX:**
  - It lives in `rhi/metal` but implements the *renderer's* `Denoiser`, a layering inversion (`mtl/MetalFxDenoiser.h:5,11`).
  - It runs RHI compute passes for prepare and resolve, and between them calls `encodeToCommandBuffer` directly on the frame command buffer (`mtl/MetalFxDenoiser.mm:500,555-558,579-582`), which needs no encoder open.
  - It CPU-writes a shared exposure texture (lines 519-529) and issues portability `Transition` calls that are no-ops on Metal.
- **UI** (`inc/RHIUiHandler.h`):
  - `Setup(render_pass)` binds the ImGui backend to one pass. On Vulkan the ImGui pipeline is built for that VkRenderPass (`vk/VulkanUi.cpp:35-66`).
  - It records raw `vkCmd*` inside the RHI render pass (line 93), which bypasses the `RHITrackedState` filter.
  - On Metal it takes the pass's encoder and descriptor (`mtl/MetalUi.mm:25-47`).
  - The single instance is cached in RHIContext (`inc/RHI.h:234-241`).

## 7. Other obstructions and helpers

**Obstructions:**

- **Global mutable state:** the backend `context` globals; the static `render_passes` list; the implicit current pass; ids and `id_dirty_`; the debug set `deleted_resources_` (`inc/RHI.h:304-306`).
- **Commands recorded as side effects of resource operations**, at whatever point the stream is in:
  - `Upload` records a copy plus transitions into the current command buffer (`vk/VulkanImage.cpp:129-151`);
  - `RHIBuffer::Upload` does a staging copy (`src/RHIBuffer.cpp:242-264`);
  - `RHIBuffer::PartialUpdate` creates a shader, a PSO, three buffers and a compute pass *per call* and dispatches (`src/RHIBuffer.cpp:29-81`);
  - `ReadToMemory` submits and waits for device idle (`src/RHIImage.cpp:7-42`).
  - Copies or transitions issued inside a render pass are invalid on both backends, and nothing asserts against them.
- **Hidden per-pass resources:** MSAA images, NRD pools, MetalFX exposure, TLAS scratch/instance buffers, `PartialUpdate` temporaries.
- **Mid-frame resource creation is legal and common:** dynamic staging buffers, `RecreateBuffer` growth, and PSO compile in `PartialUpdate`.

**Helpers:**

- **Naming:** every resource has a name. Vulkan applies object names and pass labels only when validation is enabled (`vk/VulkanContext.cpp:1086-1100`, `vk/VulkanRenderPass.cpp:147-154,243-246`); Vulkan compute passes get no label. Metal labels objects and encoders always (`mtl/MetalRHIInternal.h:13-19`).
- **Stable ids**, **exact-match RT reuse**, **frames-in-flight-delayed deletion**, and the **end-of-render task** hook.

## Implications for a render graph

**Seams to build on:**

1. **The pass bracket.** `Begin/EndRenderPass` and `Begin/EndComputePass` are already exclusive and unnested (`src/RHI.cpp:271-333`). The graph can own them. It can also inject barriers, labels and timers between passes at a single point: `RHIContext::Begin*PassInternal`.
2. **The image layout tracker.** Per-subresource `current_layout_` plus `Transition` is a workable "current state" store. The graph can compute (target layout, src stage, dst stage) from declared usage and call `Transition`, replacing the hand-written calls in the renderers.
3. **Automatic declared-I/O harvesting.**
   - Walk `GetBindings()` of each PSO recorded by a pass: sampled → read; storage → RW until access flags are reflected.
   - Take RT attachments from the pass.
   - Add `NonWritable` (SPIR-V) / `MTLBinding.access` to reflection so storage bindings split into R and W.
4. **Resource ids** as graph handle keys. **Deferred deletion and end-of-render tasks** handle history-resource and transient lifetime.
5. **The RT pool** as the seed of a transient allocator. It needs per-frame acquire/release semantics instead of init-time acquire.

**Pain points and required RHI changes:**

1. **The barrier model is incomplete on Vulkan.**
   - Same-VkLayout transitions emit nothing (`vk/VulkanImage.cpp:77-80`). You need an explicit memory/execution barrier path (image *and buffer*) that fires on access-type change, not only on layout change.
   - Track the last access and stage per subresource, not just the layout.
   - Handle `General` in `GetImageAccessFlags` (`vk/VulkanImage.h:60-63`).
   - Extend `RHIPipelineStage` with AS-build, ray-query-in-CS/PS, host and indirect-read stages. Consider moving to sync2 (Vulkan 1.3 / `VK_KHR_synchronization2`).
   - Buffers need tracked state at all.
2. **Two sources of truth for attachment layouts.** The pass attribute's initial/final layouts and the tracker disagree silently. The graph should derive pass initial/final layouts itself, or the RHI should validate `Begin` against `GetCurrentLayout`.
3. **Pass ↔ PSO ↔ RT ↔ image coupling blocks transient aliasing on Vulkan.** Framebuffers are built at pass creation, and PSOs bake the VkRenderPass and viewport. You need one of:
   - a `VkFramebuffer` cache keyed by (render pass, views);
   - `VK_KHR_imageless_framebuffer`;
   - dynamic rendering (1.3 / `VK_KHR_dynamic_rendering`).

   You also need PSOs keyed on attachment *formats* and a dynamic viewport per pass rather than per PSO. Metal already re-reads images at `Begin`, but PSOs still bake formats.
4. **No transient or memoryless memory**, no aliasing and no heaps. For mobile TBDR (Adreno, Apple), the graph's biggest win needs:
   - `TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED` (Vulkan);
   - `MTLStorageModeMemoryless` (Metal);
   - load/store op inference.

   Aliasing would need VMA aliasing and `MTLHeap`. On Metal, heap resources are untracked by default, so aliasing forces manual `MTLFence`s and turns off the auto-tracking the current Metal path relies on.
5. **No subpasses and one load/store per attachment class.** Per-attachment load/store and clear values are needed. Fix the Metal clear-color and array-layer bugs (`mtl/MetalRenderPass.mm:92-101`).
6. **Side-effect command recording.** Uploads, `PartialUpdate`, readbacks and TLAS builds record wherever they are called, and some submit or block (`src/RHIImage.cpp:28-33`, `mtl/MetalRayTracing.mm:203-222`). The graph needs explicit transfer/AS-build passes. The RHI needs "record into this command list" variants and a ban (assert) on copies or transitions inside an active pass. The blocking Metal BLAS compaction should become a pre-graph or async step.
7. **Foreign passes need a declared-I/O adapter:**
   - NRD: user IN/OUT images are known from `DispatchResource`, and its pools are private, so it fits as a single opaque compute node.
   - MetalFX: needs an "external encoder, no open encoder" node type.
   - ImGui: needs a raster node bound to a specific VkRenderPass, and must invalidate `RHITrackedState` afterwards (call `ResetCommandState` after any foreign raw recording).
   - The Vulkan TLAS build needs an AS-write → shader-read barrier node.
8. **Descriptor lifetime.** Cached descriptor sets are released at end of the current frame and can be rewritten while still in flight (`vk/VulkanDescriptorSetManager.cpp:251-268`). A graph that rebinds per frame (transient views) will stress this, so release should be delayed by frames in flight. The bindless dirty flush happens only in render-pass `Begin` (`vk/VulkanRenderPass.cpp:156`); it should move to a per-frame or per-pass-begin point that covers compute.
9. **Queues and threading.** There is one queue, one command buffer per frame and a single-threaded pool with a global `context`. Async compute or parallel recording would need per-thread pools and command lists, queue-family ownership transfers and cross-queue semaphores; none exist. A single-queue, single-thread graph fits the current RHI without change.
10. **Profiling and labels.** Add Vulkan debug labels for compute passes and when validation is off, and timers for render passes on both backends. Fix Metal frame timing under command-buffer splits. The graph pass bracket is the natural place for all of these.
