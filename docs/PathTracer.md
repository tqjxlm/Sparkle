# Path Tracers

The cpu pipeline ([CPURenderer](../libraries/source/renderer/renderer/CPURenderer.cpp)) and the gpu pipeline ([ray_trace.cs.slang](../shaders/ray_trace/ray_trace.cs.slang)) trace the same model, so the cpu path tracer is the reference for the gpu one. Denoising is covered in [Denoiser.md](Denoiser.md).

## Settings

| cvar          | default | pipelines | effect                                                                                      |
| ------------- | ------- | --------- | ------------------------------------------------------------------------------------------- |
| `bounce`      | 8       | cpu, gpu  | path segments per sample (see [Scene Model](#scene-model))                                  |
| `spp`         | 1       | cpu, gpu  | samples per pixel per frame                                                                 |
| `max_spp`     | 2048    | cpu, gpu  | samples per pixel after which accumulation stops                                            |
| `dynamic_spp` | false   | gpu       | adapt `spp` to reach `target_framerate` (default 60)                                        |
| `enable_nee`  | true    | gpu       | next event estimation (see [below](#next-event-estimation)); toggling restarts accumulation |
| `thread`      | 64      | cpu       | maximum worker threads                                                                      |

## Scene Model

* **Lights**: the sky map and emissive surfaces. Sky radiance is clamped to `CameraRenderProxy::OutputLimit` when sampled and when building the sky sampling table, so the per-sample firefly clamp does not clip HDR sky peaks only on BSDF-only paths. Emitters are two-sided and terminate a path. The directional light is not rendered by either path tracer: it is a delta light, which BSDF sampling can never reach.
* **BSDF**: the metallic-roughness model that raster lighting evaluates (`CalculateDirectLightingSplit`): a GGX specular lobe with height-correlated Smith masking and Schlick fresnel on `F0 = lerp(0.04, base_color, metallic)`, plus a lambertian lobe `(1 - F) * (1 - metallic) * base_color / pi`. Roughness is clamped to 0.08 (GGX alpha 0.0064), below which the distribution's peak overflows fp32. Dielectrics (`eta > 0`) are perfect, with delta reflection and refraction.
* **Path length**: `bounce` is the number of path segments: a path that reaches a light after `bounce` segments counts, a longer one does not.

The BSDF can be evaluated for any direction pair (`EvaluatePbrBsdf` / `PbrBxDF`). Sampling picks the specular lobe with a probability from the fresnel luminance at the view angle, samples it from the visible normal distribution or the diffuse lobe from a cosine-weighted hemisphere, and weights the direction by both lobes over their mixed pdf. A specular direction below the surface ends the path.

## Next Event Estimation

`enable_nee` adds one light sample at every non-dielectric vertex, weighted against BSDF sampling with the power heuristic. A ray that the BSDF sends into a light takes the complementary weight, so the two strategies estimate the same image. Off, the gpu path tracer is BSDF-only like the cpu one.

* **Light choice**: the sky or an emissive triangle, half and half when the scene has both.
* **Sky**: an alias table over the sky map's luminance times texel solid angle, at up to 128x128 texels per cube face, built on the CPU when the sky proxy initializes for the gpu pipeline (`SkyRenderProxy::GetSamplingTable`). A direction is uniform within its texel, so its pdf is exact for any direction, which MIS needs for escaped rays.
* **Emissive triangles**: an alias table over every emissive mesh triangle in world space, weighted by area times the material's emitted luminance averaged over its texture ([EmissiveTriangles](../libraries/include/renderer/resource/EmissiveTriangles.h)). A point is uniform on its triangle. A per-instance offset buffer maps a BSDF ray's hit to its triangle for MIS. The table rebuilds on the CPU when primitives are added, removed or moved between slots, when an emissive primitive's transform changes, or when materials are created; a moving emitter therefore rebuilds every frame.

## Validation

`gpu_nee_parity` renders the converged gpu path tracer without and with next event estimation after turning one mesh into an emitter, and requires both to carry the same energy (see [tests/rendering/nee_parity_test.py](../tests/rendering/nee_parity_test.py)). Any mismatch between `EvaluatePbrBsdf` and `SamplePbrBsdf`, or between a light's sampling and its pdf, shows up there as an energy shift.
