---
name: validate-change
description: Decide and run the validation a Sparkle change needs before it is declared finished, and handle failing reference images or goldens. Use after implementing a change and before reporting it done or committing.
---

# Validate a Change

1. Run the local gates of [docs/CONTRIBUTING.md](../../../docs/CONTRIBUTING.md#validation-gates) in their fail-fast order. The full `dev/run_tests.py` coverage run for this host is the gate; a focused `--case` run during development does not replace it.
2. Add the registry cases that cover the change but are outside this host's coverage, with `--case`:

   | change touches                              | also run                                                                                                            |
   | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
   | render graph, barriers, RHI synchronization | the `*_sync_validation` cases and `sync_validation_hazard` ([docs/Test.md](../../../docs/Test.md#validation-layer)) |
   | gpu path tracer, BSDF, light sampling       | `gpu_render_static`, `gpu_nee_parity` on a GPU with hardware ray tracing                                            |
   | denoisers                                   | `tests/denoiser/run_denoiser_gates.py` ([docs/Denoiser.md](../../../docs/Denoiser.md#tests))                        |
   | IBL cook shaders or their CPU port          | `ibl_parity` on macos; it is slow, so only then                                                                     |
   | USD loader or exporter                      | `usd_round_trip`                                                                                                    |

3. Leave the other platforms to CI instead of reproducing every triplet locally. CI cannot run converged gpu path tracing or the denoisers ([docs/CI.md](../../../docs/CI.md#hosted-runner-quirks)), so those stay local.
4. Fix every crash or error you hit. Report an unrelated one and record it in [docs/TODO.md](../../../docs/TODO.md).
5. Update the docs the change affects.

## When a Reference Fails

Update a reference only when the change intends that output change, and show the user the before and after. Otherwise the failure is a bug; debug it with the `debug-rendering` skill.

* Screenshot ground truth: [docs/Test.md](../../../docs/Test.md#ground-truth). Only the maintainer can publish it; hand over the captures.
* Render graph golden: [docs/RenderGraph.md](../../../docs/RenderGraph.md#golden-graph-shapes).
* Packaged TestScene: [docs/USD.md](../../../docs/USD.md#packaged-testscene).
