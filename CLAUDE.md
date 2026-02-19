# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

Also read: [README.md](README.md), [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md), [docs/Development.md](docs/Development.md), [docs/TODO.md](docs/TODO.md), [docs/CI.md](docs/CI.md)

Update all docs after every edit if there are high-level changes. Make all docs clean and accurate. Do not repeat contents across docs.

## Quality Ensurance

Always run build tests and functional tests to ensure quality. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) and [docs/CI.md](docs/CI.md) for details.

```bash
python build.py --framework=[glfw, macos, ios, android] --clangd           # Generate compile_commands.json
python build.py --framework=[glfw, macos, ios, android] --clean            # Clean before build
python build.py --framework=[glfw, macos, ios, android] --asan             # AddressSanitizer
```

```bash
python build.py --run --framework=[glfw, macos, ios, android] --pipeline [forward, deferred, gpu, cpu]              # Build and run
python ./dev/functional_test.py --framework [glfw, macos, ios, android] --pipeline [forward, deferred, gpu, cpu]    # Run functional test against ground truth (does not trigger building)
```

## Visual Debugging

Use --auto_screenshot=true as a run argument to automatically take a screenshot after the scene is fully loaded and a frame is fully rendered.
The screenshot is saved to generated/screenshots/ and named with the scene name and pipeline.
You can modify configs and the code to get another screenshot to visualize the changes.
You can modify the render pipeline to output different images to the screenshot for debugging.
You can modify the screenshot mechanism to capture at different timings.
Ground truth images can be found in [docs/CI.md](docs/CI.md). But if you are working on a feature that is meant to change the final image output, you should not rely on the ground truth images.

## Logs

For latest running logs, see [external storage path]/logs/output.log. Backup logs from previous runs are also stored there.

Logs will also be redirected to console when running from command line.

External storage path is platform dependent:

- Windows: executable's directory/generated/logs. e.g. build_system/glfw/output/build/generated/logs
- macOS: ~/Documents/sparkle/logs
- iOS: <App's_Document_Directory>/logs. you can use ios-deploy to pull logs.
- Android: /sdcard/Android/data/io.tqjxlm.sparkle/files/logs. you can use adb to pull logs.

## Code Style Guidelines

**Mandatory** - Apply these strictly:

- **Formatting and naming**: Follow `.clang-format`, `.clang-tidy`, `.markdownlint.json`, PEP8. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) for details
- **Modern C++20**: Use newest features (concepts, ranges, std::format, etc.)
- **Self-documenting code**: Avoid comments for obvious code; comment only structural design, algorithms, and non-obvious caveats
- **No dead code**: Remove unused code, arguments, variables, includes immediately
- **Clean design**: Prefer simplicity and readability over cleverness
- **No over-engineering**: Only implement what's requested; avoid speculative abstractions

**Review standard**: Apply Linus Torvalds-level scrutiny to all changes.

## Architecture

### Key Entry Points

| Component         | Location                                                       | Description                        |
| ----------------- | -------------------------------------------------------------- | ---------------------------------- |
| `AppFramework`    | [libraries/source/application/](libraries/source/application/) | Main application base class        |
| `RenderFramework` | [libraries/source/application/](libraries/source/application/) | Rendering pipeline lifecycle       |
| `NativeView`      | [frameworks/source/](frameworks/source/)                       | Platform windowing/input interface |
| `RHI`             | [libraries/include/rhi/](libraries/include/rhi/)               | Graphics API abstraction singleton |
| `TaskManager`     | [libraries/include/core/task/](libraries/include/core/task/)   | Async task scheduling              |

### Build System Architecture

The build system uses a factory pattern with abstract `FrameworkBuilder` interface:

- [build.py](build.py) - Main entry point, orchestrates setup and build
- [build_system/builder_interface.py](build_system/builder_interface.py) - Abstract `FrameworkBuilder` base class
- [build_system/builder_factory.py](build_system/builder_factory.py) - Creates platform-specific builders
- [build_system/prerequisites.py](build_system/prerequisites.py) - Auto-installs CMake, Ninja, Vulkan SDK
- Platform builders: `glfw/build.py`, `macos/build.py`, `ios/build.py`, `android/build.py`

**Build output:** `build_system/<platform>/output/` (artifacts), `project/` (IDE), `product/` (archives)

## Shader Development

**Pipeline:** Slang (.slang) → SPIRV → Metal (via spirv-cross)

```text
shaders/
├── include/      # Common headers
├── ray_trace/    # RT compute shaders
├── screen/       # Post-processing
├── standard/     # Vertex/pixel shaders
└── utilities/    # Utility compute
```

## Common Pitfalls

- **iOS builds**: Require `APPLE_DEVELOPER_TEAM_ID` and `--apple_auto_sign` for device deployment
- **Android builds**: Java/NDK auto-detected from Android Studio; ensure it's installed
- **Shader errors**: Check both SPIRV compilation and Metal conversion logs
- **RHI resources**: Use deferred deletion pattern for GPU resource cleanup
- **Cross-platform paths**: Use `FileManager` abstraction, never raw path separators
