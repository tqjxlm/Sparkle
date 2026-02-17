# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

Also read: [README.md](README.md), [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md), [docs/Development.md](docs/Development.md), [docs/TODO.md](docs/TODO.md)

Update all docs after every edit if there are high-level changes. Make all docs clean and accurate. Do not repeat contents across docs.

## Quick Reference

```bash
python build.py --framework=glfw --run              # Build and run (GLFW/desktop)
python build.py --framework=macos --run             # Build and run (native macOS)
python build.py --framework=ios --run               # Build and run (iOS device)
python build.py --framework=android --run           # Build and run (Android device)
python build.py --framework=glfw --clangd           # Generate compile_commands.json
python build.py --framework=glfw --clean            # Clean before build
python build.py --framework=glfw --asan             # AddressSanitizer
```

See [docs/Development.md](docs/Development.md) for full build options, environment variables, and IDE setup.

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

Shader hot-reload supported in debug builds.

## Common Tasks

**Adding a new rendering pipeline:**

1. Create pipeline class in `libraries/source/renderer/`
2. Register in pipeline factory
3. Add config option in `resources/config/config.json`

**Adding platform support:**

1. Implement `NativeView` in `frameworks/source/<platform>/`
2. Create builder in `build_system/<platform>/build.py`
3. Register in `build_system/builder_factory.py`

**Debugging build issues:**

- Build logs: `build_system/<platform>/output/build/build.log`
- Use `--clean` to clear cached state
- Use `--asan` for memory issues
- Git submodule issues: `git submodule update --init --recursive`

## Common Pitfalls

- **iOS builds**: Require `APPLE_DEVELOPER_TEAM_ID` and `--apple_auto_sign` for device deployment
- **Android builds**: Java/NDK auto-detected from Android Studio; ensure it's installed
- **Shader errors**: Check both SPIRV compilation and Metal conversion logs
- **RHI resources**: Use deferred deletion pattern for GPU resource cleanup
- **Cross-platform paths**: Use `FileManager` abstraction, never raw path separators
