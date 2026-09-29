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

* [ ] msaa
* [ ] render graph on-chip passes: merge raster passes through pixel-local reads (Vulkan dynamic rendering local read, Metal framebuffer fetch), with memoryless attachments, break reasons and a tile budget
* [ ] render graph resize: transients re-resolve each frame and persistent resources reset through a hook, instead of recreating the renderer and scene proxies
* [ ] subpass
* [ ] `MetalSampler` ignores the sampler attribute's LOD range and anisotropy
* [ ] `std::hash<RHISampler::SamplerAttribute>` hashes only the border color and address mode
* [ ] `RHIContext::GetOrCreateDummyTexture` keys its cache by a 32-bit attribute hash without an equality check
* [ ] `PickPhysicalDevice` appends the ray tracing extension list to `device_extensions_` once per candidate device, and `CheckDeviceExtensionSupport` mutates that static list (portability subset), so duplicate extension names are possible on multi-GPU hosts and on MoltenVK

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

* [ ] scene replacement has no render-command lifetime fence. Calling
      `SceneManager::LoadScene` while commands for the previous scene generation are
      still queued can invoke a destroyed component; independently destroying a scene
      with renderable components can leave removal commands holding its dead
      `SceneRenderProxy`. Loader callbacks also lack a generation token, so an older
      load can mutate the replacement scene. Scene/component teardown needs
      generation-owned render commands or an explicit drain-before-destroy contract;
      load completion must then use the same generation boundary.
