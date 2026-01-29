# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Quick Reference

**Build & Run:**

```bash
python build.py --framework=glfw --run              # Build and run (GLFW/desktop)
python build.py --framework=macos --run             # Build and run (native macOS)
python build.py --framework=ios --run               # Build and run (iOS device)
python build.py --framework=android --run           # Build and run (Android device)
```

**IDE Project Generation:**

```bash
python build.py --framework=glfw --generate_only    # Visual Studio / Ninja
python build.py --framework=macos --generate_only   # Xcode (macOS)
python build.py --framework=ios --generate_only     # Xcode (iOS)
python build.py --framework=android --generate_only # Android Studio
```

**Development Tools:**

```bash
python build.py --framework=glfw --clangd           # Generate compile_commands.json
python build.py --framework=glfw --asan             # AddressSanitizer
python build.py --framework=glfw --profile          # Tracy profiler
python build.py --framework=glfw --clean            # Clean before build
python build.py --framework=glfw --config=Release   # Release build
python build.py --framework=glfw --archive          # Archive for distribution
python build.py --cmake-args='-DFOO=bar'            # Pass CMake args
```

## Code Style Guidelines

**Mandatory** - Apply these strictly:

- **Formatting**: Use `.clang-format` for C++/Objective-C/Slang, `.markdownlint.json` for Markdown, PEP8/autopep8 for Python
- **Modern C++20**: Use newest features (concepts, ranges, std::format, etc.)
- **Self-documenting code**: Avoid comments for obvious code; comment only structural design, algorithms, and non-obvious caveats
- **No dead code**: Remove unused code, arguments, variables, includes immediately
- **Clean design**: Prefer simplicity and readability over cleverness
- **No over-engineering**: Only implement what's requested; avoid speculative abstractions

**Review standard**: Apply Linus Torvalds-level scrutiny to all changes.

## Project Overview

Sparkle is a C++20 cross-platform 3D rendering engine focused on hardware ray tracing on mobile devices. It's an educational/research project, not production-ready.

**Tech Stack:**

- Languages: C++20, Objective-C++, Python (build), Slang (shaders)
- Graphics: Vulkan (primary), Metal (Apple)
- Platforms: Windows, macOS, Linux, iOS, Android

## Architecture

### Key Entry Points

| Component         | Location                                                       | Description                        |
| ----------------- | -------------------------------------------------------------- | ---------------------------------- |
| `AppFramework`    | [libraries/source/application/](libraries/source/application/) | Main application base class        |
| `RenderFramework` | [libraries/source/application/](libraries/source/application/) | Rendering pipeline lifecycle       |
| `NativeView`      | [frameworks/source/](frameworks/source/)                       | Platform windowing/input interface |
| `RHI`             | [libraries/include/rhi/](libraries/include/rhi/)               | Graphics API abstraction singleton |
| `TaskManager`     | [libraries/include/core/task/](libraries/include/core/task/)   | Async task scheduling              |

### Directory Structure

```text
Sparkle/
├── libraries/           # Core engine code
│   ├── include/         # Headers (application, core, io, renderer, rhi, scene)
│   └── source/          # Implementations
├── frameworks/          # Platform-specific code
│   └── source/          # android, apple, glfw, macos, ios implementations
├── shaders/             # Slang shaders (include, ray_trace, screen, standard, utilities)
├── thirdparty/          # Git submodules (21 libraries)
├── build_system/        # Build scripts per platform
├── resources/           # Assets and config
└── .github/             # CI workflows and actions
```

### Build System Architecture

The build system uses a factory pattern with abstract `FrameworkBuilder` interface:

- [build.py](build.py) - Main entry point, orchestrates setup and build
- [build_system/builder_interface.py](build_system/builder_interface.py) - Abstract `FrameworkBuilder` base class
- [build_system/builder_factory.py](build_system/builder_factory.py) - Creates platform-specific builders
- [build_system/prerequisites.py](build_system/prerequisites.py) - Auto-installs CMake, Ninja, Vulkan SDK
- Platform builders: `glfw/build.py`, `macos/build.py`, `ios/build.py`, `android/build.py`

**Builder interface methods:** `configure_for_clangd()`, `generate_project()`, `build()`, `archive()`, `run()`

**Build output:** `build_system/<platform>/output/` (artifacts), `project/` (IDE), `product/` (archives)

## Rendering Pipelines

| Pipeline     | Description                   |
| ------------ | ----------------------------- |
| `cpu`        | CPU path tracer               |
| `gpu`        | GPU path tracer (RT hardware) |
| `forward`    | Forward rasterization         |
| `deferred`   | Deferred rasterization        |
| `forward_rt` | Forward with ray tracing      |

Select via: `--pipeline <name>` or `resources/config/config.json`

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

## Environment Variables

| Variable                  | Example                                                       | Required For      |
| ------------------------- | ------------------------------------------------------------- | ----------------- |
| `VULKAN_SDK`              | `/Users/x/VulkanSDK/1.4.313.0`                                | All platforms     |
| `VCPKG_PATH`              | `D:/SDKs/vcpkg`                                               | Windows GLFW      |
| `ANDROID_HOME`            | `/Users/x/AndroidSDK`                                         | Android           |
| `JAVA_HOME`               | `/Applications/Android Studio.app/Contents/jbr/Contents/Home` | Android           |
| `APPLE_DEVELOPER_TEAM_ID` | `ABC123DEF4`                                                  | iOS device builds |

The build system auto-detects many paths (Android Studio, Xcode, VS). See README for details.

## Dependencies

**External (must be installed):**

- Vulkan SDK 1.4.313.0+
- Xcode (macOS/iOS) or Visual Studio (Windows) or Android Studio (Android)
- GLFW3 (for GLFW framework only)

**Auto-managed:** CMake 3.30.5+, Ninja 1.12.1+, git submodules

**Third-party submodules (21 libraries):**
argparse, bvh, cpptrace, eigen, fast_float, hash-library, IconFontCppHeaders, imgui, ios-cmake, json (nlohmann), magic_enum, mimalloc, spdlog, spirv_reflect, stb, thread-pool, tinygltf, tinyusdz, tracy, vma, volk, xoshiro_cpp

## CI/CD

GitHub Actions CI at [.github/workflows/ci.yml](.github/workflows/ci.yml):

- Matrix: macOS/Windows × all frameworks × Debug/Release
- Auto-skips builds for docs-only PRs
- Custom actions: `setup-environment`, `setup-certs-ios`, `archive-macos`
- macOS notarization and iOS provisioning handled automatically

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
