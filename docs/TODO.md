# Todo list

## CI/CD

* [x] format and style check pipeline
* [ ] unit test pipeline
* [ ] functional test pipeline
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
* [x] render target pooling
* [ ] render graph
* [ ] subpass

## IO

* [ ] full USD support (current one is far from complete)
* [ ] USD export: analytic spheres are exported as tessellated meshes, so ray-traced
      silhouettes differ after a round trip (see docs/USD.md)
* [ ] USD export: `.usdz` / `.usdc` output (blocked on tinyusdz's experimental binary writer)
* [ ] external data loader interface

## Cook

* [ ] standalone cooker
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
* [ ] event based input handling

## Known Issues

* [ ] a shader resource declared in a C++ `USE_SHADER_RESOURCE` table but absent from the
      compiled shader (e.g. dead-code-eliminated by slang) crashes with a null dereference in
      `RHIShaderResourceSet::UpdateLayoutHash` during pipeline setup instead of failing with a
      clear error.
