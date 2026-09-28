# Platform facts for a lean cross-API render graph (Vulkan + Metal), as of 2026-09-25

Scope: the platform facts that constrain a render graph whose jobs are (a) keep intermediate data on-chip, (b) derive barriers and layout transitions automatically, (c) record commands on multiple threads. Targets: desktop Vulkan (NVIDIA/AMD), MoltenVK on macOS, Adreno 830 (S25 Ultra) and Arm Mali on Android, and native Metal (Metal 3 and Metal 4) on Apple Silicon.

How this was gathered: the primary sources are cited inline. Driver support comes from live queries of the Vulkan Hardware Database (gpuinfo.org) internal API on 2026-09-25, and the report IDs are cited. Items marked **[unverified]** had no primary source I could read. Items marked **[inference]** are my own reasoning, not something a vendor states.

---

## 1. Vulkan: subpasses, dynamic rendering, dynamic_rendering_local_read, tile extensions

### 1.1 Subpasses (VkRenderPass)

- Subpasses with `VK_DEPENDENCY_BY_REGION_BIT` dependencies and input attachments are the original Vulkan 1.0 way to keep G-buffer data in tile memory. The driver may merge the subpasses into one hardware pass ([Arm GPU Best Practices 3.4, §7.6](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)).
- Mali merges subpasses only when all of these hold. Merging saves a write-out or read-back. There are fewer than 9 unique color+input attachments across the merged subpasses (depth/stencil does not count). The depth/stencil attachment does not change. All attachments use the same sample count ([Arm BP §7.6](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)). Tile color storage is 128 bits per pixel on older Mali, up to 256 bpp from G72, and up to 1024 bpp on recent GPUs. A larger G-buffer "can be used at the expense of requiring smaller tiles", and Arm recommends a 128-bit G-buffer budget (same source).
- Measured on Mali-G76 ([Vulkan Samples "subpasses"](https://docs.vulkan.org/samples/latest/samples/performance/subpasses/README.html)): merged subpasses cut physical tiles/s from 614.7k to 262.2k (−55%). Fatter G-buffer formats broke merging (409.6k tiles/s). Forgetting TRANSIENT+LAZILY_ALLOCATED roughly doubled fragment jobs (56/s to 113/s).
- Arm now describes subpasses as implementation-dependent and unpredictable: "Whether subpasses merge depends on the implementation. The optimization depends on the hardware, the driver, and the precise API configuration." Arm also warns that merged passes can be slower clock-for-clock, delay fragment start because binning grows, and can disable transaction elimination ([Arm blog, Peter Harris, 2025-11-06](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-subpasses-the-good-the-bad-and-the-ugly)).
- Qualcomm: a properly structured render pass lets the full subpass chain execute per tile, "avoiding the need to resolve subpasses to system memory after each pass… gains of over 10% frametime". Snapdragon Profiler's Rendering Stages metric shows whether passes merged ([Qualcomm Game Developer Guide, GMEM loads](https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html); [Adreno best practices](https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html), whose page is JS-rendered, so this was read through search excerpts).
- **MoltenVK does not merge subpasses.** `beginNextSubpass()` ends the Metal render encoder, so every Vulkan subpass becomes its own Metal render pass ([MoltenVK issue #2454, 2025-02](https://github.com/KhronosGroup/MoltenVK/issues/2454); [issue #490 on transient attachments across subpasses](https://github.com/KhronosGroup/MoltenVK/issues/490)). On MoltenVK, subpass-based on-chip merging is lost, and memoryless (lazily allocated) attachments shared across subpasses are broken.

### 1.2 VK_KHR_dynamic_rendering (core 1.3)

- It replaces VkRenderPass/VkFramebuffer with `vkCmdBeginRendering`. On-chip subpass functionality was left out of scope on purpose and pushed to a separate extension. Vendors judged render-area granularity unimportant for tilers ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html)).
- Arm: plain dynamic rendering "automatically disables subpass fusion". Driver r50 fixed that with local_read ([Arm BP §7.6](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)).
- **Suspend/resume**: `VK_RENDERING_SUSPENDING_BIT` / `VK_RENDERING_RESUMING_BIT` split one render pass instance across command buffers. The pairs "must be submitted in the same batch". "No action or synchronization commands, or other render pass instances, are allowed between suspending and resuming render pass instances". `pRenderingInfo` must match apart from these flags ([spec, renderpass chapter](https://docs.vulkan.org/spec/latest/chapters/renderpass.html); [proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html)). This exists explicitly as "an alternative method of recording across multiple command buffers" to secondaries.
- Secondary command buffers use `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT` plus `VkCommandBufferInheritanceRenderingInfo` (formats, sample count, view mask). `VK_RENDERING_CONTENTS_INLINE_BIT_KHR` (needs `maintenance7` or `nestedCommandBuffer`) lets one render pass mix inline and secondary contents ([spec](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)).

### 1.3 VK_KHR_dynamic_rendering_local_read (core in 1.4, partially)

- It lets a fragment shader read attachment and storage values written by earlier fragments at the same pixel inside one dynamic render pass. "Pipeline barriers are now allowed within dynamic rendering if they include `VK_DEPENDENCY_BY_REGION_BIT`, and source and destination stages are all framebuffer-space stages." It adds the layout `VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR`. `vkCmdSetRenderingAttachmentLocationsKHR` remaps fragment output locations to attachments. `vkCmdSetRenderingInputAttachmentIndicesKHR` maps `input_attachment_index` to color/depth/stencil attachments ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html)).
- It deliberately **cannot** express: switching depth/stencil mid-pass, layout transitions or queue transfers inside the pass, reads of values not written at the same fragment location, or anything else that would force a vendor to split the pass (same proposal; [Khronos blog "Streamlining Subpasses", 2024-01-25](https://www.khronos.org/blog/streamlining-subpasses)). Shaders keep using `subpassInput`/`subpassLoad`. Shaders written for classic render passes port unmodified ([Vulkan Samples: dynamic_rendering_local_read](https://docs.vulkan.org/samples/latest/samples/extensions/dynamic_rendering_local_read/README.html); [Arm BP §7.6](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)).
- Vulkan 1.4 promotion is partial. 1.4 implementations must support local read for storage resources and single-sampled color attachments. Depth/stencil and multisampled local reads are gated by `dynamicRenderingLocalReadDepthStencilAttachments` and `dynamicRenderingLocalReadMultisampledAttachments` ([VK_VERSION_1_4 proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_VERSION_1_4.html)). Roadmap 2024 requires the extension ([Roadmap appendix](https://docs.vulkan.org/spec/latest/appendices/roadmap.html)).
- Arm's current recommendation order for Mali ([Arm blog 2025-11](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-subpasses-the-good-the-bad-and-the-ugly)):
  1. dynamic_rendering_local_read, supported since driver r49p1. The Best Practices guide says r50.
  2. `VK_EXT_rasterization_order_attachment_access`, from r36p0/r40p0.
  3. `VK_EXT_shader_tile_image`, from r44p1, which is "superseded by DRLR".
  4. Classic subpasses, as the legacy path.
- Arm caveat: `RENDERING_LOCAL_READ` is an "unsafe" layout for transaction elimination, because storage writes are outside the tile write path ([Arm BP §7.10](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)).
- The Vulkan Guide's TBR best practices call local_read "the solution" for applications migrating to dynamic rendering ([guide](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)).

**Driver support (gpuinfo.org device coverage, queried 2026-09-25):**

- **Adreno 830**: supported since the launch driver 512.800.x (API 1.3.284). All Samsung SM-S938B/U/N (S25 Ultra) reports are 512.800.1–512.800.64 on Android 15/16 with `dynamicRenderingLocalRead = true` ([report 45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218)). Newer Adreno 830 drivers (512.842+ with API 1.4.295, and 512.891.x) report the 1.4 depth/stencil and MSAA local-read properties as true ([report 52020](https://vulkan.gpuinfo.org/displayreport.php?id=52020)). OEM driver versions lag a lot: OPPO/vivo/HONOR Adreno 830 reports on Android 17 still show 512.800.80.
- **Mali**: G710/G715/G720/G925-Immortalis support it with r49–r54 drivers. `VK_EXT_shader_tile_image` goes back to r44–r46 ([coverage query](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_dynamic_rendering_local_read&platform=android)).
- **NVIDIA** (Windows, 55x+ drivers) and **AMD** (Windows AMDVLK/Adrenalin and RADV) support it. RTX 4080 at 620.12 and RX 7900 XTX both report depth/stencil+MSAA local read = true ([report 50636](https://vulkan.gpuinfo.org/displayreport.php?id=50636), [report 49290](https://vulkan.gpuinfo.org/displayreport.php?id=49290)).
- **MoltenVK**: added in MoltenVK 1.4.0 (2025-08-20) ([Whats_New](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md)). The M3 Pro report on MoltenVK 1.4.2 (driverVersion 0.2.2210) shows depth/stencil+MSAA local read = true ([report 51742](https://vulkan.gpuinfo.org/displayreport.php?id=51742)). **[inference]** Metal framebuffer fetch cannot read depth (see §3.1), so MoltenVK has to emulate depth local reads somehow. Treat them as not on-chip on Apple until measured.
- Android policy: "Devices that launch with Android 16 and higher must support Vulkan 1.4" (64-bit, non-low-memory devices) ([AOSP](https://source.android.com/docs/core/graphics/implement-vulkan)). So from Android 16 launch devices on, color local read is guaranteed.

### 1.4 VK_EXT_shader_tile_image

- It gives explicit, current-pixel-only access to tile color data (mandatory feature) and optionally depth and stencil. Reads are coherent by default. A non-coherent mode needs in-pass `vkCmdPipelineBarrier2` with memory barriers only and BY_REGION. It works only within one render pass instance ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_shader_tile_image.html)).
- Support: Mali and PowerVR only. No Adreno, NVIDIA, AMD or MoltenVK ([coverage](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_EXT_shader_tile_image&platform=android)). Arm calls it superseded by local_read.

### 1.5 Qualcomm tile extensions

- `VK_QCOM_tile_properties` queries tile size and grid. It is present on Adreno 6xx/7xx/8xx including S25 Ultra 512.800.64 ([report 45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218)).
- `VK_QCOM_tile_shading` adds per-tile execution (`vkCmdBeginPerTileExecutionQCOM`), tile-sized compute dispatch inside a render pass (`vkCmdDispatchTileQCOM`), tile-attachment access from compute/fragment, and an optional "apron" for neighbourhood reads. Using it disables FlexRender (Adreno's automatic choice between binning and direct/IMR mode) and forces TBDR. It requires tile_properties. It "builds upon" dynamic_rendering_local_read with "functionality and performance expected to be equivalent" for render pass objects and dynamic rendering. Restrictions: no stores to depth/stencil or input attachments, fragment shaders can't store color through it, and no queries or tess/geom/RT inside per-tile execution ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_QCOM_tile_shading.html)). Adreno 830 has it from 512.800.x/512.842+. The S25 Ultra's 512.800.64 does not list it.
- `VK_QCOM_tile_memory_heap` exposes a tile-memory VkMemoryHeap. Contents persist only within a submission batch. Images must be 2D, single mip/layer and non-MSAA ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_QCOM_tile_memory_heap.html)). Adreno 830 from 512.842.x only.
- The point for the graph: Qualcomm's own statement means that on Adreno, dynamic rendering plus local_read is the intended base for on-chip work. Tile shading is an optional vendor tier on top.

### 1.6 Roadmap profiles

- **Roadmap 2024** requires dynamic_rendering_local_read, load_store_op_none, maintenance5, push_descriptor, and others. **Roadmap 2026** adds robustness2, fragment_shading_rate, shader_clock, compute_shader_derivatives, cooperative_matrix, maintenance7/8/9, and more. Neither milestone lists unified_image_layouts, rasterization_order_attachment_access or shader_tile_image ([Roadmap appendix](https://docs.vulkan.org/spec/latest/appendices/roadmap.html); [Khronos blog 2026-01-23](https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension)). The latest core version is still 1.4 (spec 1.4.36x).

**Recommendation for tilers today:** the vendors agree. Arm, Qualcomm (through tile_shading's dependency) and Khronos all point to dynamic rendering plus dynamic_rendering_local_read. Subpasses remain as a legacy fallback, and on MoltenVK they are actively worse.

---

## 2. Transient attachments, load/store ops, bandwidth

### 2.1 Vulkan

- `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` means an image "may" be backed by `LAZILY_ALLOCATED` memory. Such images may carry **only** COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT and INPUT_ATTACHMENT usage (VUID-VkImageCreateInfo-usage-00963) ([spec, resources](https://docs.vulkan.org/spec/latest/chapters/resources.html)). A transient image therefore can never be sampled, used for storage, or be the target of a copy later.
- Lazily-allocated memory types (gpuinfo reports):
  - Present on Adreno 830 (memory type 7 or 8 on heap 0) ([45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218), [52020](https://vulkan.gpuinfo.org/displayreport.php?id=52020)).
  - Present on Mali (Arm BP recommends it).
  - Present on MoltenVK/Apple Silicon ([51742](https://vulkan.gpuinfo.org/displayreport.php?id=51742)). MoltenVK maps non-host-visible LAZILY_ALLOCATED to `MTLStorageModeMemoryless` on Apple Silicon ([MoltenVK source/diff via search](https://skia.googlesource.com/external/github.com/KhronosGroup/MoltenVK/+/607aaff4c12cba0f9b52db82177d7e36c452e4c2%5E!/)).
  - **Absent** on NVIDIA RTX 4080 and AMD RX 7900 XTX ([50636](https://vulkan.gpuinfo.org/displayreport.php?id=50636), [49290](https://vulkan.gpuinfo.org/displayreport.php?id=49290)). The graph must fall back to ordinary device-local memory, which it can alias.
- Arm render-pass rules ([Arm BP §7.3](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)):
  - Use `LOAD_OP_CLEAR`/`DONT_CARE` unless the algorithm needs prior contents.
  - Back single-pass attachments with TRANSIENT+LAZILY_ALLOCATED.
  - Use `STORE_OP_DONT_CARE` for dead data and `STORE_OP_NONE` for read-only attachments that must be kept.
  - Don't `vkCmdClearAttachments` inside a pass (not free).
  - Don't set load/store ops for attachments the pass doesn't need.
- Measured numbers ([Vulkan Samples "render_passes"](https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html)): CLEAR vs LOAD on color saved 600.2 MiB/s. DONT_CARE vs STORE on depth saved 554.8 MiB/s of writes. Using vkCmdClear* instead of loadOp cost about 6M extra fragment cycles/s.
- MSAA: resolve inline with `pResolveAttachments`/resolve attachments, use storeOp DONT_CARE and lazily-allocated memory for the MSAA image, and never `vkCmdResolveImage`. Example: "4x MSAA 1080p at 60 FPS requires 3.9GB/s … compared to 500MB/s when using an inline resolve". Subpasses with different MSAA levels cannot fuse ([Arm BP §7.5](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695); [Vulkan Guide TBR](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)). `VK_EXT_multisampled_render_to_single_sampled` is widely available on Adreno/Mali ([coverage](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_EXT_multisampled_render_to_single_sampled&platform=android)).
- `LOAD_OP_NONE`/`STORE_OP_NONE` (VK_KHR_load_store_op_none) are core in 1.4 and on Roadmap 2024. Adreno 830 has both the EXT and KHR variants ([45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218)).
- Adreno terminology: GMEM Load = "unresolve" and GMEM Store = "resolve". Both are expensive per tile and are driven by loadOp/storeOp ([Qualcomm GMEM loads](https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html)).
- Mali transaction elimination needs single-sample, single-mip COLOR_ATTACHMENT images **without** TRANSIENT usage. Moving an image from a safe layout to an unsafe one (UNDEFINED, GENERAL, RENDERING_LOCAL_READ) invalidates its signature buffer ([Arm BP §7.10](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)). Measured: using the last known layout instead of UNDEFINED roughly doubled CRC-killed tiles and cut write bandwidth by about 10% ([Vulkan Samples "layout_transitions"](https://docs.vulkan.org/samples/latest/samples/performance/layout_transitions/README.html)).

### 2.2 Metal

- `MTLStorageModeMemoryless` is tile memory, textures only (no buffers), and only for the life of one render pass. Its contents "can't [be accessed] with load or store" actions ([MTLStorageMode.memoryless](https://developer.apple.com/documentation/metal/mtlstoragemode/memoryless); [choosing storage modes](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus)). Supported on all Apple GPU families (Apple2+) ([Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)).
- `MTLStoreActionDontCare` lets the GPU discard contents. "Some GPUs may still store the contents back… you can't rely on that" ([doc](https://developer.apple.com/documentation/metal/mtlstoreaction/dontcare)). Load and store actions run per tile and are the only times render targets touch memory. "Alpha Blending will always happen on Tile Memory" ([WWDC20 "Harness Apple GPUs with Metal"](https://developer.apple.com/videos/play/wwdc2020/10602/)).
- Savings. **[inference]** A 1080p D32 depth buffer is 1920×1080×4 B ≈ 8.3 MB, and 4x MSAA is 4× that. Digital Legends saved about 60 MB of footprint by marking a G-buffer memoryless ([WWDC19-606 transcript](https://asciiwwdc.com/2019/sessions/606)). MSAA resolve happens from tile memory, so MSAA color can be memoryless as well ([WWDC20-10602](https://developer.apple.com/videos/play/wwdc2020/10602/)).
- Metal 4 removed "store-action options" (`MTLStoreActionOptions`) because "they don't apply to Apple silicon GPUs". Plain load and store actions remain ([Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)).

---

## 3. Metal: render pass model, tile features, hazards, Metal 4

### 3.1 Render pass/encoder model and on-chip features

- One `MTLRenderCommandEncoder` is one render pass with fixed attachments and load/store actions. Tile memory persists only within that encoder. Apple GPUs overlap the fragment tail of one pass with the vertex stage of the next ([TBDR guide](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering)).
- **Programmable blending** (framebuffer fetch, `[[color(n)]]` fragment inputs) is available on all Apple families (Apple2+). It is not available on Intel/AMD Macs ([Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf); [WWDC20-10631](https://developer.apple.com/videos/play/wwdc2020/10631/)). "Programmable Blending allows fragment shaders to access pixel data directly from Tile Memory. This allows you to merge multiple render passes into one" ([WWDC20-10602](https://developer.apple.com/videos/play/wwdc2020/10602/)).
- Imageblocks, tile shaders and raster order groups are Apple4+ (A11+), so all Metal 4 hardware has them. Imageblocks persist "for the lifetime of a tile, across draws and dispatches". Tile shaders mix compute into a render pass. In a fragment shader "the current fragment has access to only the imageblock data associated with that fragment's position" ([TBDR guide](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering)).
- Limits from the [Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf):
  - At most 8 color render targets.
  - Implicit imageblock (attachment) budget per pixel per sample is 128 B on Apple7+ (M1/A14+) and 64 B on Apple4–6.
  - Tile size is 32×32 (32×16 at 4x MSAA).
  - Valid explicit tile sizes are 32×32, 32×16 and 16×16 ([tileWidth doc](https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/tilewidth)).
- **No fragment-to-fragment barriers inside a render pass on Apple GPUs**: "Metal doesn't support intrapass barriers that wait for the tile or fragment stages on devices that have a TBDR architecture… if a tile dispatch needs results from another tile, or a fragment shader needs results from another fragment, then start a new [pass]" ([Synchronizing stages within a pass](https://developer.apple.com/documentation/metal/synchronizing-stages-within-a-pass)). The Feature Set Tables footnote says the same for `memoryBarrier(scope:after:before:)` with `.fragment`/`.tile`. Attachment loads and stores, raster-order-group accesses and tile dispatches are ordered per pixel/tile automatically ([Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)). The Vulkan by-region barrier inside a local-read pass therefore has **no Metal counterpart and needs none**.
- **Slang** lowers `SubpassInput` to Metal framebuffer fetch. `[[vk::input_attachment_index(N)]]` always maps to `[[color(N)]]`. Metal framebuffer fetch "reads from color attachments only, not from [[depth]]/[[stencil]]". SubpassInput must be a global declaration referenced from the fragment entry (not in a ParameterBlock, and helpers need `[ForceInline]`). `SubpassInputMS` per-sample reads are unsupported ([Slang Metal target doc](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-02-metal-target-specific.html)). A search summary said this arrived in Slang v2026.9 (May 2026) **[unverified]**, so check the pinned Slang version.

### 3.2 Hazard tracking, fences, events, heaps (Metal 3 model)

- By default Metal automatically tracks hazards for resources created from `MTLDevice` (tracked). Heap resources default to untracked. Tracking has runtime overhead, and untracked resources need barriers, fences or events ([MTLHazardTrackingMode](https://developer.apple.com/documentation/metal/mtlhazardtrackingmode); [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)).
- Scopes, from smallest to largest: intrapass barrier, then `MTLFence` (across passes in a queue), then intraqueue consumer/producer barriers, then `MTLEvent` (across queues), then `MTLSharedEvent` (CPU and other devices). "Select the synchronizing mechanism with smallest scope" ([Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)). Apple GPUs honour fences per stage, so the vertex stage can run while fragment waits. The producer must be committed before the consumer ([MTLFence](https://developer.apple.com/documentation/metal/mtlfence)). Consumer queue barriers also work with Metal 3 encoders ([consumer barriers](https://developer.apple.com/documentation/metal/synchronizing-passes-with-consumer-barriers)).
- Aliasing: `makeAliasable()` works only on automatic-allocator heaps. Once a resource is aliased it "can't be un-aliased or moved", and reading it afterwards is undefined. Use `MTLEvent`/`MTLFence` so aliases are never accessed concurrently. For fine-grained control use `MTLHeapType.placement` and manage offsets yourself ([makeAliasable](https://developer.apple.com/documentation/metal/mtlresource/makealiasable())).

### 3.3 Metal 4 (WWDC25; Apple7+ = A14/M1 and later)

- `MTL4CommandBuffer` and `MTL4CommandAllocator` are created from the device and are independent of queues. They can be encoded in parallel and reused indefinitely (`beginCommandBuffer(allocator:)`). An allocator is bound to one command buffer at a time and is `reset()` once the GPU finishes the frame. Command buffers do **not** retain resources ([Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api); [WWDC25-205](https://developer.apple.com/videos/play/wwdc2025/205/)).
- `MTL4CommandQueue.commit:count:` submits an array in order. Work from any thread is sent to the GPU at commit ([core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)). Events interoperate between MTL4 and legacy queues.
- **No hazard tracking at all.** "In Metal 4, the framework considers all resources untracked", and `hazardTrackingMode` has no effect on MTL4 queues ([core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api); [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)). The unified `MTL4ComputeCommandEncoder` (compute+blit+AS) runs its commands **concurrently** unless barriers intervene ([WWDC25-254](https://developer.apple.com/videos/play/wwdc2025/254/)).
- Barriers are stage-to-stage (`MTLStageVertex/Fragment/Tile/Object/Mesh/Dispatch/Blit/AccelerationStructure/MachineLearning`):
  - Intrapass: `barrier(afterEncoderStages:beforeEncoderStages:visibilityOptions:)`.
  - Consumer queue barrier at the start of the consuming pass: `barrier(afterQueueStages:beforeStages:…)`.
  - Producer queue barrier in the producing pass: `barrier(afterStages:beforeQueueStages:…)`, which is Metal-4-only.

  Visibility options are `none` (execution-only), `device` (flush to the device coherence point) and `resourceAlias` (make aliased virtual addresses consistent) ([consumer barriers](https://developer.apple.com/documentation/metal/synchronizing-passes-with-consumer-barriers); [producer barriers](https://developer.apple.com/documentation/metal/synchronizing-passes-with-producer-barriers); [MTL4VisibilityOptions](https://developer.apple.com/documentation/metal/mtl4visibilityoptions)). Apple: "It's important to choose appropriate stages to avoid over-synchronizing" ([WWDC25-254](https://developer.apple.com/videos/play/wwdc2025/254/)).
- **Suspend/resume render passes**: `MTL4RenderEncoderOptionSuspending/Resuming`. Requirements: commit the whole chain in one `commit:count:` array, with the first command buffer only suspending, the last only resuming, and the middle ones both. There must be no compute, blit, AS or ML work between them. This "conceptually replaces `MTLParallelRenderCommandEncoder`" ([MTL4RenderEncoderOptions](https://developer.apple.com/documentation/metal/mtl4renderencoderoptions); [core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)). **This is structurally the same rule as Vulkan's suspend/resume (same batch, nothing in between).**
- Color attachment mapping: one MTL4 render encoder holds all attachments and remaps logical shader outputs to physical attachments, so no second encoder is needed ([WWDC25-205](https://developer.apple.com/videos/play/wwdc2025/205/)). This is the counterpart of `vkCmdSetRenderingAttachmentLocations`.
- Residency sets are required: add every resource, including drawables through `CAMetalLayer.residencySet`, attached to the queue once or per command buffer. `MTL4ArgumentTable` replaces per-slot binding. Texture view pools give lightweight views. Placement sparse resources are available ([WWDC25-254](https://developer.apple.com/videos/play/wwdc2025/254/)).
- What Metal 4 changes for the graph: on Metal 3 the graph can lean on tracked resources, so hazard derivation is optional there. On Metal 4 the graph **must** emit barriers, residency and retention (lifetime) itself, just as on Vulkan, minus image layouts.

### 3.4 Metal multithreaded encoding (Metal 3)

- `MTLParallelRenderCommandEncoder` splits one render pass across threads. Its sub-encoders execute in creation order ([doc](https://developer.apple.com/documentation/metal/mtlparallelrendercommandencoder)).
- For cross-pass parallelism, call `enqueue()` on several `MTLCommandBuffer`s in the desired order, encode them on worker threads, and commit in any order ([enqueue()](https://developer.apple.com/documentation/metal/mtlcommandbuffer/enqueue())).

### 3.5 Vulkan on Apple: MoltenVK vs KosmicKrisp

- MoltenVK 1.4.x is "not a fully conformant Vulkan implementation" and supports Intel and Apple Silicon. KosmicKrisp (LunarG, Mesa) is Vulkan 1.3 conformant, requires Metal 4 and Apple Silicon, and ships in the Vulkan SDK ([LunarG, Jan 2026](https://www.lunarg.com/the-state-of-vulkan-on-apple-jan-2026/)). On gpuinfo, the Apple reports that expose unified_image_layouts and nested_command_buffer are KosmicKrisp (driver 26.x), not MoltenVK ([coverage](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_unified_image_layouts&platform=macos)).

---

## 4. Multithreaded command recording in Vulkan

- **Pools**: one per thread per frame in flight. NVIDIA recommends "L * T + N pools". Reset whole pools rather than individual buffers, and don't create or destroy pools ([NVIDIA Vulkan Dos and Don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/)). Arm: use `ONE_TIME_SUBMIT`, avoid `RESET_COMMAND_BUFFER_BIT` pools and `SIMULTANEOUS_USE`, call `vkResetCommandPool` periodically. Arm ignores `TRANSIENT_BIT` and `vkTrimCommandPool` ([Arm BP §4.6–4.7](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)). Measured on Android: pool reset cost 0.45% of frame time, individual resets 1.16%, and allocate/free 28.8% ([Vulkan Samples command_buffer_usage](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)).
- **Secondaries on mobile**:
  - Arm: hardware before Mali-G710 "does not have native support for invoking commands in a secondary command buffer", so they carry extra CPU cost. Arm still expects apps to use them for multithreaded render pass construction and asks to "minimize the number of secondary command buffer invocations" ([Arm BP §4.8](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)).
  - Qualcomm: secondaries disabled LRZ only before Adreno 650. Snapdragon 865 and newer don't ([Adreno best practices](https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html), via search excerpt).
  - For the target GPUs (Adreno 830, Mali G710+), secondaries are acceptable in moderate numbers.
- **Parallel primaries**: record one primary per pass or pass group and submit them in order in a single `vkQueueSubmit2`. Arm's sample compares this with secondaries on Mali-G72. Both gave gains (1.57× for multithreading overall), and secondaries dropped frame time a bit further ([Vulkan Samples multithreading_render_passes](https://docs.vulkan.org/samples/latest/samples/performance/multithreading_render_passes/README.html)). Parallel primaries cannot split a single render pass. Inside a pass the options are secondaries or dynamic-rendering suspend/resume.
- NVIDIA: "Don't record tiny command buffers". Build them evenly across cores, and allocate/begin/end on the thread that records. Secondaries "can be helpful… check carefully" ([NVIDIA](https://developer.nvidia.com/blog/vulkan-dos-donts/)).
- **VK_EXT_nested_command_buffer**: on Android it appears only on Adreno 8xx 512.891.x and Mesa Turnip, not on S25 Ultra 512.800.64. NVIDIA supports it with unlimited nesting, AMD with nesting level 1 ([50636](https://vulkan.gpuinfo.org/displayreport.php?id=50636), [49290](https://vulkan.gpuinfo.org/displayreport.php?id=49290)). MoltenVK doesn't have it. Don't depend on it.
- **Unreal**: the Render Thread builds platform-agnostic RHI command lists, and "anything generated in parallel on the frontend is translated in parallel on the backend" for Vulkan and D3D12 ([UE 5.8 Parallel Rendering Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/parallel-rendering-overview-for-unreal-engine)). The public doc does not say whether Vulkan uses secondaries or split primaries. I believe UE's VulkanRHI uses secondaries for parallel work inside a render pass **[unverified; source requires Epic GitHub access]**. Granite (Maister) merges passes into subpasses under the conditions "share attachments, ≤1 depth, BY_REGION-only dependencies", treats single-pass CLEAR/DONT_CARE attachments as transient, and aliases non-overlapping lifetimes ([Render graphs and Vulkan, a deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)).

---

## 5. Barriers, layouts, async compute

### 5.1 synchronization2 and stage masks

- sync2 is core 1.3 and universal on the targets ([coverage](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_synchronization2&platform=android)). NVIDIA: "Group barriers in one call to vkCmdPipelineBarrier2", "Minimize the use of barriers. A barrier may cause a GPU pipeline flush", avoid read-to-read barriers, use precise stages, and use UNDEFINED when contents aren't needed ([NVIDIA](https://developer.nvidia.com/blog/vulkan-dos-donts/)). AMD: "produce data early and wait late", and the driver may widen but never narrow your masks ([GPUOpen, Vulkan barriers explained](https://gpuopen.com/learn/vulkan-barriers-explained/)).
- **Tilers**: Mali has two hardware slots, vertex/compute/transfer-buffer and fragment. Forward dependencies (vertex/compute → fragment) are cheap. **Backward** dependencies (fragment → vertex/compute) create bubbles. Never use BOTTOM→TOP, ALL_GRAPHICS→ALL_GRAPHICS or ALL_COMMANDS→ALL_COMMANDS. For render pass to render pass use `srcStage=ALL_GRAPHICS, dstStage=FRAGMENT_SHADER`. Don't set an event and then wait on it immediately. Don't use semaphores within one queue ([Arm BP §3.9](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)). Measured: COLOR_ATTACHMENT_OUTPUT→FRAGMENT_SHADER instead of →VERTEX_SHADER removed bubbles and cut frame time by 13% on Mali ([Vulkan Samples pipeline_barriers](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html)). The Vulkan Guide warns that broad barriers "might force the GPU to finish all pending fragment work before it can even start the binning pass" ([TBR guide](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)).
- **Split barriers (events)**: NVIDIA recommends `vkCmdSetEvent2`/`vkCmdWaitEvents2` for asynchronous barriers. AMD says they are useful only with enough work between set and wait. Arm says not to wait right after setting ([NVIDIA](https://developer.nvidia.com/blog/vulkan-dos-donts/); [GPUOpen](https://gpuopen.com/learn/vulkan-barriers-explained/); [Arm BP §3.9](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)). Metal has no split-barrier equivalent. The closest are fences, updated early and waited late.

### 5.2 VK_KHR_unified_image_layouts (2025)

- It guarantees that `GENERAL` is as efficient as the specific layouts wherever it is valid. Transitions are still needed from UNDEFINED/PREINITIALIZED (initialization), to and from PRESENT_SRC/SHARED_PRESENT, and for video unless `unifiedImageLayoutsVideo` is set. Attachment feedback loops go through `VkAttachmentFeedbackLoopInfoEXT`. "Image barriers are still required for best performance on some hardware, even if both src and dst layouts are GENERAL." Memory barriers are unaffected ([proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_unified_image_layouts.html); [Khronos blog 2025-06-26](https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation)). It is headed for core but is not in Roadmap 2026.
- Support (gpuinfo 2026-09-25):
  - NVIDIA Windows/Linux: yes (RTX 4080 at 620.12).
  - RADV: yes. AMD Windows: **no** (RX 7900 XTX 2.0.395) ([coverage windows](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_unified_image_layouts&platform=windows)).
  - Adreno 830: yes from 512.849 (Vulkan 1.4 drivers). **Not on S25 Ultra's 512.800.64.**
  - Mali: only the new Mali-G2-Ultra driver r56. **Not** G7xx/G9xx.
  - MoltenVK: no. KosmicKrisp: yes ([coverage android](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_unified_image_layouts&platform=android)).
- **[inference]** It removes most transitions on desktop only. On Mali it is unavailable, and GENERAL is also a TE-"unsafe" layout, so precise layouts still pay off there (§2.1). The graph should still derive layouts and collapse them to GENERAL when `unifiedImageLayouts` is on.

### 5.3 Async compute

- **Adreno**: an LPAC (Low Priority Async Compute) queue runs concurrently with the graphics pipe at lower priority. It suits latency-tolerant compute "on the scale of multiple milliseconds" and has a slightly larger instruction cache ([Adreno best practices](https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html), via search excerpt). S25 Ultra exposes three queue families (family 0 with 3 queues) ([45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218)).
- **Mali**: compute shares the vertex/binning slot. A separate queue lets FRAGMENT→COMPUTE post-processing overlap and gave a modest win (21.8 ms vs 22.9 ms). Gains are small because both slots share shader cores. Avoid FRAGMENT→COMPUTE barriers unless there is a plan for the COMPUTE→FRAGMENT return ([Vulkan Samples async_compute](https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html)).
- **Apple**: without barriers, work in a single queue already overlaps (Metal 4 is "concurrency by default"; vertex overlaps compute unless you over-synchronize). Multiple queues need MTLEvent ([WWDC25-254](https://developer.apple.com/videos/play/wwdc2025/254/); [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)).
- **NVIDIA**: don't overlap graphics-queue compute with async-queue compute on pre-Ampere ([NVIDIA](https://developer.nvidia.com/blog/vulkan-dos-donts/)).
- Conclusion: queue assignment can stay an optional pass attribute. Single-queue scheduling with precise stage masks gets most of the overlap on tilers.

---

## 6. Memory aliasing of transient resources

- **Vulkan spec**: memory is aliased when bound to several resources at once. Two images interpret the memory consistently only if they have identical creation parameters, both use `VK_IMAGE_CREATE_ALIAS_BIT`, and they are bound identically. Otherwise, treat a new alias's contents as undefined. Linear and non-linear resources that are adjacent and used at the same time must be separated by `bufferImageGranularity` (Adreno reports 1, RTX 4080 1024, MoltenVK 16) ([spec, Memory Aliasing / bufferImageGranularity](https://docs.vulkan.org/spec/latest/chapters/resources.html); reports [45218](https://vulkan.gpuinfo.org/displayreport.php?id=45218), [50636](https://vulkan.gpuinfo.org/displayreport.php?id=50636), [51742](https://vulkan.gpuinfo.org/displayreport.php?id=51742)).
- **VMA**: allocate with size = max, alignment = max and memoryTypeBits = AND of all members, then bind with `vmaBindImageMemory2` at offsets or use `vmaCreateAliasingImage2`. "Treat a resource after aliasing as uninitialized". Issue a barrier so users of img1 and img2 don't overlap on the GPU. Transition the new alias from `UNDEFINED`. Check that memoryTypeBits actually overlap ([VMA resource aliasing](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/resource_aliasing.html)).
- **Metal**: use heaps with `makeAliasable()` (automatic heaps, one-way) or placement heaps (manual offsets). Guard with fences/events ([makeAliasable](https://developer.apple.com/documentation/metal/mtlresource/makealiasable())). In Metal 4, barriers with `MTL4VisibilityOptions.resourceAlias` make aliased addresses consistent ([MTL4VisibilityOptions](https://developer.apple.com/documentation/metal/mtl4visibilityoptions)).
- **Gains**: Frostbite FrameGraph at 720p used 147 MB of transient memory without aliasing, 80 MB aliased on DX12 PC, and 76–77 MB on consoles. Rules: the first operation on a newly aliased resource must be a Discard or Clear, to initialise metadata. Use precise barriers ([GDC 2017 FrameGraph, slides](https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495); [GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)). The Vulkan analogue of Discard is oldLayout=UNDEFINED plus loadOp CLEAR/DONT_CARE. On DCC/compressed hardware that is what initialises metadata **[inference]**.
- **Interaction with transient/memoryless [inference]**: an attachment that lives inside one merged physical pass on a tiler takes no memory when it is lazily allocated or memoryless, so it needs no aliasing. Aliasing matters for resources that cross pass boundaries (sampled post-process chains, compute intermediates) and for desktop, which has no lazy memory type.

---

## 7. Capability matrix

Legend: ✔ supported/recommended · ~ partial or caveated · ✘ not available. Driver facts come from gpuinfo as of 2026-09-25. "Adreno 830" means the S25 Ultra's Samsung driver 512.800.64 unless noted.

| Feature | Vulkan desktop (NVIDIA / AMD) | MoltenVK 1.4.x (Apple Silicon) | Adreno 830 (Vulkan) | Mali G710+ (Vulkan) | Metal 3 (Apple Silicon) | Metal 4 (A14/M1+) |
| --- | --- | --- | --- | --- | --- | --- |
| On-chip multi-step pass primitive | IMR: no tile memory. local_read / subpasses become ordinary barriers | local_read in one Metal pass | subpasses or dynamic rendering + local_read | local_read (r49/r50+) > subpasses | one render encoder + programmable blending / imageblocks | same as Metal 3 (MTL4RenderCommandEncoder) |
| Subpass merging | n/a (no tiles) | ✘ each subpass is a new Metal pass | ✔ (>10% frametime per Qualcomm) | ~ unpredictable; merge rules (≤8 attachments, same depth/MSAA) | n/a | n/a |
| dynamic_rendering_local_read | ✔ incl. depth/MSAA | ✔ (1.4.0+); depth reads **[inference: emulated]** | ✔ ext on 512.800; 1.4 props true on 512.842+ | ✔ r49+ | n/a → `[[color(n)]]` | n/a → `[[color(n)]]` |
| Fragment reads depth on-chip | ✔ (no gain) | ~ | ✔ | ✔ | ✘ framebuffer fetch is color-only; copy depth to a color RT | ✘ same |
| shader_tile_image | ✘ | ✘ | ✘ | ✔ (superseded) | n/a | n/a |
| Rasterization-order access / ROG | ✘ (Mesa only) | ✘ | ✔ EXT | ✔ EXT/ARM | ✔ raster order groups | ✔ |
| Compute inside render pass | ✘ | ✘ | ~ QCOM_tile_shading (not on 512.800.64; 512.842+) | ✘ | ✔ tile shaders | ✔ tile shaders |
| Transient / memoryless attachment | ~ usage allowed, **no LAZILY type** → alias instead | ✔ LAZILY → memoryless | ✔ LAZILY type | ✔ LAZILY type | ✔ memoryless | ✔ memoryless |
| LOAD/STORE_OP_NONE | ✔ | ✔ | ✔ | ✔ (1.4/ext) | n/a (Load/DontCare) | n/a |
| Inline MSAA resolve | ✔ | ✔ | ✔ + MSRTSS | ✔ + MSRTSS | ✔ StoreAndMultisampleResolve | ✔ |
| Intra-pass fragment→fragment barrier | ✔ BY_REGION | ~ maps to framebuffer-fetch ordering | ✔ BY_REGION | ✔ BY_REGION | ✘ must split the pass (implicitly ordered per pixel) | ✘ same |
| Automatic hazard tracking | ✘ | ✘ | ✘ | ✘ | ✔ tracked resources (default); untracked in heaps | ✘ none |
| Explicit barriers | sync2 | sync2 | sync2 | sync2 | fences / consumer barriers | consumer/producer/intrapass barriers + fences |
| unified_image_layouts | NVIDIA ✔ · AMD Win ✘ · RADV ✔ | ✘ (KosmicKrisp ✔) | ✘ on 512.800; ✔ 512.849+ | ✘ (G2-Ultra r56 only) | n/a (no layouts) | n/a |
| Parallel recording inside one pass | secondaries / suspend-resume | secondaries / suspend-resume | secondaries (LRZ OK ≥A650) / suspend-resume | secondaries (native ≥G710) / suspend-resume | MTLParallelRenderCommandEncoder | suspend/resume across MTL4CommandBuffers |
| Parallel recording across passes | parallel primaries, ordered submit | same | same | same | enqueue() order + parallel command buffers | parallel MTL4CommandBuffers, commit:count: |
| Nested command buffers | ✔ NV (∞) / AMD (1) | ✘ | ✘ (512.891+ only) | ✘ | n/a | n/a |
| Memory aliasing | VMA (granularity 1024 on NV) | ✔ (granularity 16) | ✔ (granularity 1) | ✔ | heaps + makeAliasable / placement heaps | placement heaps + `resourceAlias` barriers |
| Async compute value | ✔ (NV pre-Ampere caveat) | ~ | ~ LPAC low-priority queue | ~ small (+5%) | single queue overlaps already | same, concurrency by default |

---

## 8. Design constraints for a cross-API render graph

### C1. Make the pixel-local read its own access type, and treat it as the only thing that allows merging

A pass declares, per resource, one of: attachment write (color or depth), attachment read-only (depth test), pixel-local read (input attachment / framebuffer fetch), sampled read, storage read/write, or transfer. A pixel-local read is legal only for data written at the same pixel, since both local_read and Metal forbid neighbour reads within a pass ([local_read proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html); [Apple intrapass](https://developer.apple.com/documentation/metal/synchronizing-stages-within-a-pass)). Any sampled read of an attachment written in the current merged group forces a split. This gives one rule for all backends.

### C2. The merge ("physical pass") rules are the intersection of vendor rules

Merge consecutive raster passes when all of these hold:

- Same render area and extent.
- Same sample count (Arm, and MSAA levels can't fuse).
- The same depth/stencil attachment or none. local_read cannot switch depth, and Arm won't merge if depth changes.
- ≤ 8 unique color+input attachments. This is Vulkan 1.4's maxColorAttachments, Metal's 8 RTs, and Arm's "<9".
- Only pixel-local dependencies between the members.
- Optionally, a bits-per-pixel budget: 128 bits/pixel keeps full 16×16 Mali tiles. Apple7+ allows 128 B/pixel/sample. On Adreno, query with `VK_QCOM_tile_properties`.

Lift the external dependencies of every member to the start of the merged pass (Granite). Keep a debug switch that disables merging, so results can be A/B'd for correctness and profiled per Arm's "profile, don't assume" advice.

### C3. Lower a merged pass once per backend

- **Vulkan (all targets: Adreno 830 512.800+, Mali r49+, NVIDIA, AMD, MoltenVK 1.4+)**: one `vkCmdBeginRendering` whose attachments are the union of the members. Between members, call `vkCmdSetRenderingAttachmentLocations` / `vkCmdSetRenderingInputAttachmentIndices` and a `vkCmdPipelineBarrier2` with memory barriers only and `BY_REGION` (framebuffer-space stages, COLOR_ATTACHMENT_WRITE → INPUT_ATTACHMENT_READ). Pixel-locally read attachments use `RENDERING_LOCAL_READ` (or GENERAL when unified layouts are on) for the whole pass. There are no in-pass layout transitions.
- **Fallback if `dynamicRenderingLocalRead` is absent**: split into separate passes with sampled reads, not VkRenderPass subpasses. **[inference]** The fallback population is small (pre-r49 Mali, very old Adreno drivers), subpasses are unpredictable on Mali and harmful on MoltenVK, and a second on-chip lowering path doubles the test matrix. Revisit only if a target device lacks local_read.
- **Metal (3 and 4)**: one render encoder with all the attachments. A pixel-local read becomes a `[[color(n)]]` fragment input (Slang `SubpassInput`). No barrier is emitted, because per-pixel ordering is implicit. Metal 4 color attachment mapping plays the role of attachment locations.
- **Binding convention [inference]**: Slang maps `input_attachment_index(N)` to `[[color(N)]]`. Make the graph assign each pixel-local input the **same index as its color attachment slot**, and use the identity mapping in `vkCmdSetRenderingInputAttachmentIndices`. Then one shader source works on both APIs.
- **Depth as pixel-local input is not portable**: Metal can't framebuffer-fetch depth. Either write linear depth into a color attachment during the G-buffer step, or have depth consumers sample it after a pass split. Validate this in the graph, as a compile error for Metal-bound pipelines.

### C4. Derive transient storage from lifetime, with backend capability fallbacks

A resource is "memoryless-eligible" when:

- its whole lifetime is inside one merged physical pass,
- its first access is CLEAR or DONT_CARE,
- nothing reads it after the pass (storeOp DONT_CARE), and
- its only usages are attachment or pixel-local input. This matches the Vulkan TRANSIENT usage restriction and Metal's memoryless restriction on load/store.

Lower it to TRANSIENT+LAZILY_ALLOCATED where that memory type exists (Adreno, Mali, MoltenVK) and to `MTLStorageModeMemoryless` on Metal. Where no lazy type exists (NVIDIA/AMD desktop), fall back to a normal image in the aliasing pool. Treat MSAA color/depth that is resolved inline as memoryless-eligible, and resolve inside the pass through resolve attachments or `StoreAndMultisampleResolve`. Never use a separate resolve copy.

### C5. Derive load and store ops, never hand-author them

- loadOp: CLEAR when the pass declares a clear. DONT_CARE (Metal: DontCare) when the previous contents are dead, including the first use after aliasing. LOAD only when a producer exists and the render area doesn't fully overwrite.
- storeOp: STORE if any later consumer exists, including next frame (history resources are graph-external and imported). DONT_CARE if dead. NONE for read-only depth or other untouched attachments that must persist (Vulkan 1.4). On Metal use DontCare, since Metal has no NONE.

This is the single largest bandwidth lever. The Arm samples measured about 600 MiB/s per attachment at 60 fps.

### C6. Barrier and hazard derivation happens once, at compile time, into a backend-neutral barrier list at pass boundaries

- Each barrier entry is (src stages, src access, dst stages, dst access, resource or global, Vulkan-only old/new layout, aliasing flag). Emit barriers at physical pass boundaries and batch them per boundary: one `vkCmdPipelineBarrier2` in Vulkan, one consumer queue barrier at the start of the consuming pass in Metal 4, fences in Metal 3 heaps.
- Keep masks precise. Prefer forward dependencies. Warn in debug builds on fragment→vertex/compute dependencies (Arm §3.9), and on wide masks such as ALL_COMMANDS or BOTTOM→TOP.
- Use global memory barriers for buffers. Use image barriers only where a layout changes or a queue ownership transfer happens.
- Layouts: track the last layout per image. Collapse to GENERAL when `unifiedImageLayouts` is on (NVIDIA, RADV, newer Adreno). Otherwise keep precise layouts, which Mali TE rewards. Use UNDEFINED only when the previous contents are dead (first use after aliasing, transient). Keep persistent color targets in "safe" layouts on Mali.
- Skip split barriers (events) in v1. Arm and AMD caveat their use, Metal has no equivalent, and a compile-time scheduler that orders independent passes between producer and consumer gets most of the overlap. **[inference]**
- Metal 3 backend: allocate graph resources from heaps as untracked and emit fences, rather than relying on tracked-resource overhead. This keeps the Metal 3 and Metal 4 code paths structurally identical (fence or barrier at pass boundaries). **[inference]**

### C7. Multithreading model: "record chunks", with ordering decided at compile time

- The graph compiles to an ordered list of physical passes. A pass (or a group of passes) is a record unit, and a large raster pass can be split into N chunks. Workers record chunks in parallel. Barriers are precomputed, so recording threads never derive hazards.
- Across passes: Vulkan uses one primary per record unit and `vkQueueSubmit2` in graph order. Metal 3 uses `enqueue()` in graph order and parallel encoding. Metal 4 uses one MTL4CommandBuffer per unit and `commit:count:` in order.
- Within one raster pass, both APIs offer the same rule, so model a **suspend/resume chunk chain**: Vulkan `SUSPENDING/RESUMING` in one submit batch, Metal 4 `Suspending/Resuming` in one commit array, with nothing between chunks. **Consequence**: a merged pass's internal BY_REGION barriers and attachment-location changes must stay inside a single chunk, and chunk boundaries may fall only between draws of the same member step. Vulkan secondaries (`RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`) are the equivalent fallback and are fine on Adreno ≥650 and Mali ≥G710 in moderate counts. On Metal 3 use `MTLParallelRenderCommandEncoder`. **[unverified]** How tiler drivers perform on suspend/resume vs secondaries has no public vendor data. Measure both on S25 Ultra before choosing a default.
- Pools and allocators: one Vulkan command pool per (thread, frame in flight), reset per frame. One MTL4CommandAllocator per (thread, frame in flight), `reset()` after that frame's completion. Use ONE_TIME_SUBMIT. Avoid tiny chunks (NVIDIA) and minimize secondary count (Arm).
- Metal 4 command buffers don't retain resources, and residency sets are mandatory. The graph's resource pool must keep transient and aliased allocations alive until the frame's completion (timeline value or event), and must keep a residency set containing all pool heaps.

### C8. Aliasing pool for resources that cross pass boundaries

- Linear-scan the lifetimes in compiled order and place the members into a small number of large allocations: VkDeviceMemory through VMA with merged requirements and offsets, or Metal placement heaps. Placement heaps give the same offset model as Vulkan. makeAliasable is one-way and cannot model the reuse pattern cleanly **[inference]**.
- Respect `bufferImageGranularity` when mixing buffers and optimal images (1024 on NVIDIA).
- On first use of each alias, emit UNDEFINED plus CLEAR/DONT_CARE (Vulkan) or a DontCare/Clear load action or explicit clear (Metal). Emit a barrier between the last use of alias A and the first use of alias B: a Vulkan memory dependency, or a Metal 4 barrier with `resourceAlias` visibility.
- Expected gain is roughly 45% of transient memory (Frostbite: 147 to 80 MB). Tiler attachments that are memoryless already cost 0 MB and stay out of the pool.

### C9. Queues

Keep `queue` as an optional pass attribute that defaults to graphics. Add async compute later only for measured wins: Adreno LPAC for long, latency-tolerant compute, and Mali for overlapping fragment-bound post-processing. On Apple, keep a single queue and rely on precise stage barriers.

### C10. Capability tiers to query at startup (Vulkan)

- `dynamicRenderingLocalRead`, plus the two 1.4 depth/MSAA properties when the driver reports 1.4.
- A lazily-allocated memory type.
- `unifiedImageLayouts`.
- `synchronization2` (required).
- `maintenance7` / `nestedCommandBuffer` (optional).
- `VK_QCOM_tile_properties`, for the bits-per-pixel budget.

The S25 Ultra on Samsung's 512.800.64 driver has local_read (including depth/MSAA through the extension), lazy memory, rasterization-order access and tile_properties. It **lacks** unified_image_layouts, tile_shading, tile_memory_heap and nested command buffers. Design so that this is the baseline mobile tier.

---

## Open questions and uncertainties

- Whether MoltenVK's local read of depth/stencil (reported as supported) stays on-chip. Metal framebuffer fetch is color-only, so this needs measuring.
- Suspend/resume vs secondaries on Adreno 830 and Mali: there is no vendor guidance. Profile it (Snapdragon Profiler "Rendering Stages" shows pass merges and GMEM loads).
- Which Slang version is pinned in the project, and whether it has the Metal `SubpassInput` lowering (reported as v2026.9, unverified).
- The Qualcomm best-practices page is JS-rendered. Its quotes here (LRZ/secondaries, LPAC, >10% subpass gain) come from search-engine excerpts of the official page, not a direct read.
