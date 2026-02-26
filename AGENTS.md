# AGENTS.md

This file provides guidance to AI coding agents when working with this repository.

* Read [README.md](README.md) for general background of this project and the location of other docs.
* Read other docs on demand.

## Test Driven Development

* Always run build tests and functional tests to ensure quality. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) and [docs/CI.md](docs/CI.md) for details.
* Always make sure you have the ability to test with confidence before actual implementation. If in doubt, ask the user to clarify.
* Create new test cases that exactly test against new features or reproduce a bug. Improve existing test cases when necessary.
* Do not ignore any crashes or errors. If it is not related to current task, report it and record it to [docs/TODO.md](docs/TODO.md). Otherwise fix them before continuing.

See [docs/Build.md](docs/Build.md) for all build commands.

See [docs/Run.md](docs/Run.md) for run arguments, log location, path conventions, etc..

See [docs/Test.md](docs/Test.md) for how to run tests, how to write test, and test tricks.

## Visual QA

* Use `--test_case screenshot` to take a screenshot after the scene is fully loaded and a frame is fully rendered.
* Use `--test_case multi_frame_screenshot` to take 5 frames of screenshots after the scene is fully loaded and a frame is fully rendered. This is useful for temporal analysis.
* Screenshots are saved to [external-storage-path]/screenshots/ and named with the scene name and pipeline. For [external-storage-path], refer to [docs/Run.md](docs/Run.md).
* By outputting different textures to render target (e.g. via "--debug_mode"), you can test any intermediate textures or render targets in the pipeline.
* When debugging visual results, you should analyze screenshot images both semantically and statistically.
* Ground truth images can be found in [docs/CI.md](docs/CI.md). But if you are working on a feature that is meant to change the final image output, you should not rely on the ground truth images.

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
