# Todo list

## CI/CD

* [ ] unit test pipeline
* [ ] performance test pipeline

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

* [ ] msaa
* [ ] render graph resize: transients re-resolve each frame and persistent resources reset through a hook, instead of recreating the renderer and scene proxies
* [ ] `MetalSampler` ignores the sampler attribute's LOD range and anisotropy
* [ ] `std::hash<RHISampler::SamplerAttribute>` hashes only the border color and address mode
* [ ] `RHIContext::GetOrCreateDummyTexture` keys its cache by a 32-bit attribute hash without an equality check
* [ ] `PickPhysicalDevice` appends the ray tracing extension list to `device_extensions_` once per candidate device, and `CheckDeviceExtensionSupport` mutates that static list (portability subset), so duplicate extension names are possible on multi-GPU hosts and on MoltenVK
* [ ] a Vulkan present on a lost or out-of-date surface waits on a per-image semaphore that `VulkanContext::ReleaseRenderResources` destroys right after `vkDeviceWaitIdle`, which does not cover the presentation engine's waits; `VK_KHR_swapchain_maintenance1` present fences would tell when the semaphore is free
* [ ] `VulkanTLAS::Update` rewrites the host-visible instance buffer in place while the previous frame's refit may still read it

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

* [ ] a windowed glfw run on macOS crashes in `glfwGetMonitorContentScale` (`GLFWNativeView::InitGUI`) when `glfwGetPrimaryMonitor` returns no monitor, e.g. in a session whose display is asleep; the content scale needs a fallback

* [ ] scene replacement has no render-command lifetime fence. Calling
      `SceneManager::LoadScene` while commands for the previous scene generation are
      still queued can invoke a destroyed component; independently destroying a scene
      with renderable components can leave removal commands holding its dead
      `SceneRenderProxy`. Loader callbacks also lack a generation token, so an older
      load can mutate the replacement scene. Scene/component teardown needs
      generation-owned render commands or an explicit drain-before-destroy contract;
      load completion must then use the same generation boundary.
