# Documentation

Start with [Architecture](Architecture.md) for how the code is laid out, then pick the topic you need.

## Using and Developing

| doc                             | covers                                                                     |
| ------------------------------- | -------------------------------------------------------------------------- |
| [Build](Build.md)               | prerequisites, `build.py`, pipeline stages, IDE setup, build internals     |
| [Run](Run.md)                   | `run.py`, storage paths and logs, the config system and general settings   |
| [Architecture](Architecture.md) | layers, repository map, entry points, threading model                      |
| [Test](Test.md)                 | the test system, running and writing tests, ground truth, platform runners |
| [CI](CI.md)                     | the CI pipeline, its generator, hosted runner quirks                       |
| [Contributing](CONTRIBUTING.md) | workflow, validation gates, coding style, documentation rules              |
| [TODO](TODO.md)                 | planned work and known issues                                              |

## Subsystems

| doc                            | covers                                                                |
| ------------------------------ | --------------------------------------------------------------------- |
| [Render Graph](RenderGraph.md) | how renderers record frames: passes, derived barriers, merging, dumps |
| [Path Tracer](PathTracer.md)   | the cpu and gpu path tracers: BSDF, next event estimation, settings   |
| [Denoiser](Denoiser.md)        | NRD and MetalFX denoising for the gpu path tracer                     |
| [Cooking](Cooking.md)          | build-time asset processing, artifacts, content images, compression   |
| [Input](Input.md)              | the input pipeline, gestures and key arbitration                      |
| [USD](USD.md)                  | USD import and export, the packaged TestScene                         |
