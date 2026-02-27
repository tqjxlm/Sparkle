# Todo list

## CI/CD

* [ ] format and style check pipeline
* [ ] unit test pipeline
* [ ] functional test pipeline
* [ ] performance test pipeline

## Path Tracing Renderers

* [ ] ASVGF
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
* [ ] render target pooling
* [ ] render graph
* [ ] subpass

## IO

* [ ] full USD support (current one is far from complete)
* [ ] external data loader interface

## Cook

* [ ] standalone cooker
* [ ] standalone shader compiler
* [ ] texture compression
* [ ] slang support for metal

## Build

* [ ] Linux support

## Infrastructure

* [ ] modularize core libraries
* [ ] rhi thread
* [ ] event based input handling

## Known Issues

* [ ] Intermittent headless GPU screenshot run failure (`sparkle.exe` exits with code `4294967295`) observed during repeated `dev/reblur_test_suite.py` baseline step (`--pipeline gpu --spatial_denoise false`); rerun with identical args can pass. Collect crash dump/log evidence and isolate root cause.
* [ ] Intermittent headless GPU denoiser-on smoke failure (`sparkle.exe` exits with code `4294967295`) observed in `dev/reblur_test_suite.py` S0.1 step (`--pipeline gpu --spatial_denoise true --test_case smoke`); rerun with identical args can pass. Collect crash dump/log evidence and confirm whether it shares root cause with the baseline intermittent crash.
* [ ] `dev/functional_test.py --pipeline gpu --spatial_denoise true` currently fails against existing GPU ground truth after Phase 4 Module I because denoiser output is no longer pass-through (`Mean FLIP error: 0.4236` on 2026-02-27, threshold `0.03`). Revisit denoiser-on ground-truth/update policy in Phase 7 quality-gate work.
