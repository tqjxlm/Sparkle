# AGENTS.md

This file provides guidance to AI coding agents when working with this repository.

* Read [README.md](README.md) for general background of this project and the location of other docs.
* Read other docs on demand.

## Test Driven Development

* Always run build tests and functional tests to ensure quality. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) and [docs/CI.md](docs/CI.md) for details.
* Create new test cases that exactly test against new features or reproduce a bug. Improve existing test cases when necessary.
* Always make sure you have the ability to test with confidence before actual implementation. If in doubt, ask the user to clarify.
* Do not ignore any crashes or errors. If it is not related to current task, report it and record it to [docs/TODO.md](docs/TODO.md). Otherwise fix them before continuing.

See [docs/Build.md](docs/Build.md) for all build commands.

See [docs/Run.md](docs/Run.md) for run arguments, log location, path conventions, etc..

See [docs/Test.md](docs/Test.md) for how to run tests, how to write test, and how to test visual output.

## Visual Debugging Methodology

* Semantic analysis is the primary way of visual debugging: always read the screenshot like a human first and evaluate its overall quality and correctness. Use statistical metrics only to back a semantic judgement and to solve the problem, never as a target in themselves — metrics are easily limited or misleading.
* Rank errors by perceptual salience, not by screen area or mean magnitude. A sharp high-frequency artifact (silhouette halo/contour, firefly, edge fringe) the eye locks onto can cover <1% of pixels yet be the most objectionable error on screen, while a smooth low-frequency shift over half the frame reads as acceptable. **An error is never too small just because of its percentage on screen** — never use pixel-count or a mean/total deviation to down-rank a localized error.
* Debug artifacts evidence-gated, never "guess a fix then ask if it looks better": **(1)** show the defect cleanly and directly (per-pixel overlay/diff at the failure's resolution, simplest reproducing setting) and get the user to confirm it's the right artifact; **(2)** **the gate is that agreed visualization** — the fix succeeds only when its highlighted defect pixels are **eliminated** (regenerate the same overlay after the fix), agreed before implementing. **Quantitative metrics are NOT the gate for rendering artifacts — they give false confidence** (a metric can show a big "% win" while the agreed image still shows the artifact); a number only *supports* the semantic verdict, never replaces it, and the image wins on disagreement. **Push back if asked for a purely quantitative criterion** — it conflicts with semantic-first. See [docs/RenderingValidation.md](docs/RenderingValidation.md) "Evidence-gated debugging".
* Full methodology and debugging techniques — how to validate a rendering feature ("a proxy is not ground truth", failure-mode-driven acceptance, signed-diff-vs-ground-truth, inspecting intermediate passes, reading raw shader values) with a worked post-mortem — are in [docs/RenderingValidation.md](docs/RenderingValidation.md). Read it before adding a rendering feature or chasing an image artifact. Screenshot / debug-view / diffing mechanics are in [docs/Test.md](docs/Test.md).

## Code Style Guidelines

* Apply strictly [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)
* Apply Linus Torvalds-level scrutiny to all changes.
* Always use formatters to format any change when possible. Do not format manually!

## Documentation

* Update all docs after every edit if there are high-level changes.
* Keep all docs clean and accurate.
* Do not repeat contents across docs.
* Put documentation in dedicated files. Avoid putting everything in AGENTS.md.

## Common Pitfalls

* **iOS builds**: Require `APPLE_DEVELOPER_TEAM_ID` and `--apple_auto_sign` for device deployment
* **Android builds**: Java/NDK auto-detected from Android Studio; ensure it's installed
* **Shader errors**: Check both SPIRV compilation and Metal conversion logs
* **RHI resources**: Use deferred deletion pattern for GPU resource cleanup
* **Cross-platform paths**: Use `FileManager` abstraction, never raw path separators
* **Debug runs**: Always use build.py to run the project during development for robustness. Do not run binaries directly.
