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

* Semantic-first: read the screenshot like a human before computing statistics; metrics only back a semantic judgement, never replace it.
* Rank errors by perceptual salience, not screen area or mean magnitude — a sharp localized artifact covering <1% of pixels can be the worst error on screen.
* Debug artifacts evidence-gated: first confirm the defect with the user via a per-pixel visualization, then gate the fix on that same agreed visualization — never on a quantitative metric alone. Push back if asked for a purely quantitative criterion.
* The full methodology — proxy-vs-ground-truth pitfalls, the validation checklist, debugging techniques — is in [docs/RenderingValidation.md](docs/RenderingValidation.md). Read it before adding a rendering feature or chasing an image artifact. Screenshot capture mechanics are in [docs/Test.md](docs/Test.md).

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
