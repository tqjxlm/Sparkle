# Todo list

## CI/CD

* [ ] unit test pipeline
* [ ] performance test pipeline

## Path Tracing Renderers

* [ ] vendor-specific denoising and super sampling
* [ ] dynamic scene

## Rasterization Renderers

* [ ] CSM
* [ ] PCSS
* [ ] AO
* [ ] ray traced shadow
* [ ] ray traced AO
* [ ] ray traced reflection

## RHI

* [ ] msaa
* [ ] render graph
* [ ] subpass

## IO

* [ ] USD export: `.usdz` / `.usdc` output (blocked on tinyusdz's experimental binary writer)
* [ ] external data loader interface

## Cook

* [ ] standalone shader compiler
* [ ] texture compression
* [ ] compile ray_trace shaders slang->metal directly and drop the spirv-cross stage.
      Blocked on slang emitting invalid MSL for bindless resource arrays
      (<https://github.com/shader-slang/slang/issues/11970>) and the entry-point
      out-parameter ICE (<https://github.com/shader-slang/slang/issues/11969>).
      All other shaders already compile slang->metal directly.

## Build

* [ ] Linux support

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
* [ ] a shader resource declared in a C++ `USE_SHADER_RESOURCE` table but absent from the compiled shader (e.g. dead-code-eliminated by slang) crashes with a null dereference in `RHIShaderResourceSet::UpdateLayoutHash` during pipeline setup instead of failing with a clear error.
* [ ] on Android, backgrounding and foregrounding the app races swapchain recreation against the render loop: `APP_CMD_INIT_WINDOW` schedules `RecreateSurface`/`RecreateSwapChain` while the render thread may sit in `VulkanContext::BeginFrame`, and the Adreno driver segfaults inside `vkWaitForFences`/`VulkanSwapChain::Recreate` roughly every other cycle (S25 Ultra, validation layer loaded). Window recreation needs to fence out the in-flight frame before the swapchain is replaced.
