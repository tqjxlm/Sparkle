# Todo List

## CI/CD

* [ ] unit test pipeline
* [ ] performance test pipeline
* [ ] an iOS simulator CI test can hang until the 900 s timeout with stdout ending at "Render thread started." (`forward_render_scale_change`); the cause is unconfirmed because it does not reproduce locally, and `build_system/ios/build.py` writes every thread's stack of a hung app to `logs/sample_<pid>.log`

## Path Tracing Renderers

* [ ] DLSS Ray Reconstruction as an integrated provider on NVIDIA RTX GPUs, through NGX on Vulkan
* [ ] FSR quality gates and per-pass timing: `run_denoiser_gates.py` and `denoiser_compare.py` take no upscaler, and FSR logs no `PassTimingAggregator` stages
* [ ] dynamic scene
* [ ] directional light: neither path tracer renders it, since only next event estimation can reach a delta light and the cpu path tracer has none

## Rasterization Renderers

* [ ] CSM
* [ ] PCSS
* [ ] AO
* [ ] ray traced shadow
* [ ] ray traced AO
* [ ] ray traced reflection

## Render Graph

Each item waits for its first user or for a profile that justifies it.

* [ ] MSAA: a `ResolveTo(src, dst)` access lowered to Vulkan resolve attachments and Metal `MultisampleResolve`, with a memoryless MSAA source
* [ ] history API: a keyed `graph.History(key, desc) -> {current, previous}` for the first ping-pong user (TAA, temporal AO)
* [ ] transient mips, layers and cube maps; graph buffers beyond copies (uniform, storage, indirect, transient); Copy passes that blit, clear or generate mips; indirect dispatch
* [ ] heap aliasing of transients, once dumped transient bytes on mobile justify aliasing barriers and untracked Metal heaps
* [ ] async compute scheduled from dependency levels, and parallel recording of physical-pass spans into separate command buffers
* [ ] Metal 4 backend lowering the barrier plan to stage barriers over untracked resources
* [ ] compile caching by graph hash
* [ ] read-only depth: `RHIStoreOp::None` lowering to `STORE_OP_NONE` and a read-only depth layout, so a `DepthTest` attachment synchronizes as a read
* [ ] unified image layouts: `GENERAL` where `VK_KHR_unified_image_layouts` exists (desktop only; Mali's transaction elimination needs precise layouts), folding later readers of one write into its first barrier
* [ ] a builder access for writes outside shaders, so the MetalFX scaler output needs no shader-write usage
* [ ] move Vulkan `Upload`/`UploadFaces` and the present transition out of backend code, so tracked states match across backends before the first graph frame
* [ ] tile budget on Adreno via `VK_QCOM_tile_properties` or per-vendor limits, once `render_graph_tile_budget_split` measurements show a split wins
* [ ] viewer: resource-version nodes instead of dense pass-to-pass edges; per-pass output thumbnails
* [ ] a per-signature ImGui Vulkan pipeline cache, which needs a public `ImGui_ImplVulkan_CreatePipeline` upstream

## IO

* [ ] USD export: `.usdz` / `.usdc` output (blocked on tinyusdz's experimental binary writer)
* [ ] external data loader interface

## Cook

* [ ] standalone shader compiler
* [ ] drop the NRD cook's SPIR-V 1.5 to 1.4 version rewrite and its `vulkan1.1spv1.4` validation target ([cook_nrd_shaders.py](../shaders/nrd/cook/cook_nrd_shaders.py)) at the next NRD recook
* [ ] compile `ray_trace` shaders from Slang to Metal directly and drop spirv-cross, blocked on Slang emitting invalid MSL for bindless resource arrays ([slang#11970](https://github.com/shader-slang/slang/issues/11970)) and an entry-point out-parameter ICE ([slang#11969](https://github.com/shader-slang/slang/issues/11969))

## Infrastructure

* [ ] modularize core libraries
* [ ] rhi thread

## Known Issues

* [ ] `scene_replacement` times out after its 3000-frame budget on a local macOS Release run on an M5 Max, about 0.3 s after its first check passes, while it passes on the CI runners; the budget counts frames, not time
