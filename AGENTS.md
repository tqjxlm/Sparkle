# AGENTS.md

This file provides guidance to AI coding agents when working with this repository.

* Read [README.md](README.md) for general background of this project and the location of other docs.
* Read other docs on demand.

## Test Driven Development

* Always run build tests and functional tests to ensure quality. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) and [docs/CI.md](docs/CI.md) for details.
* Always write tests for newly implemented features. Before implementing anything, figure out how to test it. Before fixing any bug, figure out how to reproduce it.

See [docs/Build.md](docs/Build.md) for all build and run commands.

## Visual QA

* Use --auto_screenshot=true as a run argument to automatically take a screenshot after the scene is fully loaded and a frame is fully rendered. This is the main method of visual VA.
* The screenshot is saved to generated/screenshots/ and named with the scene name and pipeline.
* You can modify configs and the code to get another screenshot to visualize the changes.
* You can modify the render pipeline to output different images to the screenshot for debugging.
* You can modify the screenshot mechanism to capture at different timings or capture multiple times in a run.
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
* ./dev/functional_test.py does not trigger building. If any code change is made, run build.py first
