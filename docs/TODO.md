# Todo list

## CI/CD

* [ ] unit test pipeline
* [ ] performance test pipeline
* [ ] `dev/run_tests.py --framework android` on a macOS host resolves to `macos-android-release`, which has no coverage column, so a coverage run against a USB device needs the `ubuntu-android-release` column
* [ ] `gpu_render_static` measures FLIP 0.0203 on the S25 Ultra, over its 0.02 gate (residual path-tracing noise on the bottle and the speaker grille)

## Path Tracing Renderers

* [ ] vendor-specific denoising and super sampling
* [ ] dynamic scene
* [ ] the GPU path tracer samples material textures with nearest filtering
* [ ] GPU path tracer MIS: an emissive surface a bounce reaches takes the sky light's MIS weight although next event estimation never samples emissive surfaces (its shadow ray treats them as occluders), so emissive light is too dark with NEE on; weight only sky misses
* [ ] GPU path tracer MIS: the NEE weight pairs the light pdf with the pdf of the BSDF's own sampled direction instead of the BSDF pdf at the light direction, so the two strategies' weights do not sum to 1; needs a BSDF pdf evaluation

## Rasterization Renderers

* [ ] CSM
* [ ] PCSS
* [ ] AO
* [ ] ray traced shadow
* [ ] ray traced AO
* [ ] ray traced reflection

## RHI

* [ ] the Metal sampler ignores `RHISampler` anisotropy and `min_lod`/`max_lod` (`MetalSampler` sets neither `maxAnisotropy` nor the LOD clamps), so Metal samples with anisotropy 1 and the full mip range where Vulkan honours them
* [ ] the glfw CMake cache keeps `Vulkan_LIBRARY` from the first configure, so after a Vulkan SDK bump an existing build still links (and on macOS bundles MoltenVK from) the old SDK until it is reconfigured from scratch
* [ ] MoltenVK 1.4.1 (Vulkan SDK 1.4.350) adds memoryless textures to its residency set, so the glfw build aborts under `MTL_DEBUG_LAYER=1` on the render graph's memoryless attachments; fixed in MoltenVK 1.4.2, bump the SDK once it ships it

* [ ] render graph MSAA: a `ResolveTo(src, dst)` access lowered to Vulkan resolve attachments and Metal `MultisampleResolve` store actions, with a memoryless MSAA source
* [ ] render graph heap aliasing of transients, once dumped transient bytes on mobile justify its aliasing barriers and untracked Metal heaps
* [ ] render graph async compute as a per-pass attribute, scheduled from dependency levels
* [ ] render graph Metal 4 backend: lower the barrier plan to Metal 4 stage barriers over untracked resources
* [ ] render graph history API: a keyed `graph.History(key, desc) -> {current, previous}` with the first ping-pong user (TAA, temporal AO)
* [ ] `MetalSampler` ignores the sampler attribute's LOD range and anisotropy
* [ ] `std::hash<RHISampler::SamplerAttribute>` hashes only the border color and address mode
* [ ] `RHIContext::GetOrCreateDummyTexture` keys its cache by a 32-bit attribute hash without an equality check
* [ ] `PickPhysicalDevice` appends the ray tracing extension list to `device_extensions_` once per candidate device, and `CheckDeviceExtensionSupport` mutates that static list (portability subset), so duplicate extension names are possible on multi-GPU hosts and on MoltenVK
* [ ] a Vulkan present on a lost or out-of-date surface waits on a per-image semaphore that `VulkanContext::ReleaseRenderResources` destroys right after `vkDeviceWaitIdle`, which does not cover the presentation engine's waits; `VK_KHR_swapchain_maintenance1` present fences would tell when the semaphore is free
* [ ] `VulkanTLAS::Update` rewrites the host-visible instance buffer in place while the previous frame's refit may still read it
* [ ] `MetalResourceArray::Bind` patches the bindless argument buffer in place while frames in flight may still read it
* [ ] Metal frame GPU time (`GPUEndTime - GPUStartTime` of the frame's last command buffer) undercounts when a TLAS build commits the frame mid-way
* [ ] three `VkSampler`s leak at `vkDestroyDevice`
* [ ] the ray tracing extension list still enables buffer device address, descriptor indexing, `VK_KHR_spirv_1_4` and float controls, which are core in Vulkan 1.2; dropping buffer device address also needs `VmaAllocatorCreateInfo::vulkanApiVersion`, which is left at 1.0
* [ ] a depth attachment a pass only tests (`DepthTest`) synchronizes as a depth write and stores with `DontCare`; a distinct `RHIStoreOp::None` lowering to `STORE_OP_NONE` and a read-only depth layout would let it synchronize as a read
* [ ] Vulkan `Upload`/`UploadFaces` and `EndFrame`'s present transition change tracked image state in backend code, so tracked states differ between Vulkan and Metal until the first graph frame; the transitions belong in common code
* [ ] the MetalFX scaler output carries shader-write usage only so the graph can declare the scaler's write; an `RGBuilder` access for writes outside shaders would remove it

## IO

* [ ] USD export: `.usdz` / `.usdc` output (blocked on tinyusdz's experimental binary writer)
* [ ] external data loader interface

## Cook

* [ ] standalone shader compiler
* [ ] drop the NRD cook's SPIR-V 1.5 to 1.4 version rewrite and its `vulkan1.1spv1.4` validation target (`shaders/nrd/cook/cook_nrd_shaders.py`) at the next NRD recook
* [ ] texture compression
* [ ] compile ray_trace shaders slang->metal directly and drop the spirv-cross stage.
      Blocked on slang emitting invalid MSL for bindless resource arrays
      (<https://github.com/shader-slang/slang/issues/11970>) and the entry-point
      out-parameter ICE (<https://github.com/shader-slang/slang/issues/11969>).
      All other shaders already compile slang->metal directly.

## Infrastructure

* [ ] modularize core libraries
* [ ] rhi thread

## Known Issues

* [ ] the control panel's Save Screenshot and Save Graph Dump build their names from `RenderFramework::render_config_.pipeline` on the main thread while the render thread's `NewFrame` writes `render_config_`
* [ ] MoltenVK's 16x anisotropic sampling is not deterministic from frame to frame on the Apple Paravirtual GPU, so a rare frame samples mip-mapped materials slightly differently; the merge-parity cases run with sampler anisotropy off

* [ ] the window scale that converts a desktop frame buffer resize into the output resolution is read once at startup (`backingScaleFactor`, the monitor content scale), so moving the window to a display with another scale sets the wrong output size
* [ ] Android reports no frame buffer resize: a new native window (`APP_CMD_INIT_WINDOW`) recreates the swap chain but keeps the output aspect ratio, which only the landscape orientation lock keeps correct; multi-window and freeform sizes stretch

* [ ] a windowed glfw run on macOS crashes in `glfwGetMonitorContentScale` (`GLFWNativeView::InitGUI`) when `glfwGetPrimaryMonitor` returns no monitor, e.g. in a session whose display is asleep; the content scale needs a fallback

* [ ] scene replacement has no render-command lifetime fence. Calling
      `SceneManager::LoadScene` while commands for the previous scene generation are
      still queued can invoke a destroyed component; independently destroying a scene
      with renderable components can leave removal commands holding its dead
      `SceneRenderProxy`. Loader callbacks also lack a generation token, so an older
      load can mutate the replacement scene. Scene/component teardown needs
      generation-owned render commands or an explicit drain-before-destroy contract;
      load completion must then use the same generation boundary.
