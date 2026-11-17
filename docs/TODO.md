# Todo list

## CI/CD

* [ ] unit test pipeline
* [ ] performance test pipeline
* [ ] `gpu_render_static` measures FLIP 0.0203 on the S25 Ultra, over its 0.02 gate (residual path-tracing noise on the bottle and the speaker grille)
* [ ] an iOS simulator CI test can hang until the 900 s timeout with stdout ending at "Render thread started." (seen once in `forward_render_scale_change`); `StartRenderThread` signalling under the mutex it waits on closes the lost wakeup that matches that stop point, but the hang never reproduces locally, so the cause is unconfirmed; `build_system/ios/build.py` writes every thread's stack of a hung app to `logs/sample_<pid>.log`

## Path Tracing Renderers

* [ ] vendor-specific denoising and super sampling
* [ ] dynamic scene
* [ ] GPU path tracer MIS: the NEE weight pairs the light pdf with the pdf of the BSDF's own sampled direction instead of the BSDF pdf at the light direction, so the two strategies' weights do not sum to 1; fixing it also needs NEE to evaluate the BSDF that `BxdfMetal` samples instead of `CalculateDirectLightingSplit`, because with weights that sum to 1 the two models' difference shifts the image's energy past the `gpu_nee_parity` gate
* [ ] the GPU path tracer traces `bounce + 1` path segments where the CPU path tracer traces `bounce`, so at one `bounce` setting the two differ wherever light arrives after many bounces (contact shadows)

## Rasterization Renderers

* [ ] CSM
* [ ] PCSS
* [ ] AO
* [ ] ray traced shadow
* [ ] ray traced AO
* [ ] ray traced reflection

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

* [ ] MoltenVK's 16x anisotropic sampling is not deterministic from frame to frame on the Apple Paravirtual GPU, so a rare frame samples mip-mapped materials slightly differently; the merge-parity cases run with sampler anisotropy off

* [ ] render tasks still queued when the render thread stops are discarded, so a material whose removal task is among them keeps its render proxy and trips `ASSERT(!render_proxy_)` in a Debug build
