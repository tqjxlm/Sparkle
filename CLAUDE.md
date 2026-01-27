# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Sparkle** is a modern, cross-platform 3D rendering engine written in C++20 that focuses on exploring hardware ray tracing capabilities, particularly on mobile devices. It serves as an educational and research demonstration project rather than a production-ready engine.

### Project Purpose

- **Educational Tool**: Demonstrates modern rendering techniques and cross-platform development
- **Research Platform**: Explores hardware ray tracing on mobile devices (iOS, Android)
- **Cross-Platform Engine**: Supports Windows, macOS, Linux, iOS, and Android
- **Modern C++**: Showcases C++20 features with clean, well-structured architecture

### Key Features

- **Multiple Rendering Pipelines**: CPU path tracer, GPU path tracer, forward, deferred, and ray tracing variants
- **Hardware Ray Tracing**: Utilizes RT cores on modern GPUs for real-time ray tracing
- **Cross-Platform Shaders**: Slang shading language with HLSL → SPIRV → Metal pipeline
- **Mobile Focus**: Optimized for iOS and Android with Metal/Vulkan backends
- **Physical-Based Rendering**: Modern PBR materials with image-based lighting
- **Development Tools**: Hot-reload, profiling, debugging, and comprehensive logging

### Technical Stack

- **Languages**: C++20, Objective-C++, Python (build system), Slang (shaders)
- **Graphics APIs**: Vulkan (primary), Metal (Apple platforms)
- **Platforms**: iOS, Android, macOS, Windows, Linux
- **Build System**: CMake with Python orchestration
- **Dependencies**: 20+ third-party libraries via git submodules

## Build System

Sparkle uses CMake with platform-specific build scripts. All build operations are now centralized through a unified Python build script that handles setup, configuration, and building automatically.

### Centralized Build System

**Unified Build Command (Required):**

```bash
python build.py --framework=<platform> [options]
```

**Platform Options:**

- `glfw` - Cross-platform GLFW build
- `macos` - Native macOS application
- `ios` - iOS application  
- `android` - Android APK

**Common Build Options:**

```bash
# Basic build commands
python build.py --framework=glfw --run          # Setup, build and run
python build.py --framework=glfw --config=Debug # Setup and debug build
python build.py --framework=macos --run         # Setup, build and run macOS app
python build.py --framework=ios --run           # Setup, build and run iOS app (requires device)

# Project generation for IDEs
python build.py --framework=macos --generate_only  # Generate Xcode project only
python build.py --framework=ios --generate_only    # Generate iOS Xcode project only
python build.py --framework=glfw --generate_only   # Generate native project only

# Setup and development
python build.py --setup_only                    # Run setup only (no build)
python build.py --framework=glfw --clangd       # Generate compile_commands.json for clangd

# Build configuration options
python build.py --framework=glfw --config=Release  # Release build
python build.py --framework=glfw --asan            # Enable AddressSanitizer
python build.py --framework=glfw --profile         # Enable profiler
python build.py --framework=glfw --shader_debug    # Enable shader debug info
```

**Android-Specific Commands:**

```bash
python build.py --framework=android            # Setup and build APK only
python build.py --framework=android --run      # Setup, build, install, and run APK
python build.py --framework=android --config=Release  # Setup and release build
```

### Automatic Setup

The build system automatically handles:

- Git submodule initialization (`git submodule update --init --recursive`)
- Resource setup (downloads external assets)
- Android validation layer setup
- Gradle wrapper download (for Android builds)
- Environment validation

### Platform-Specific Notes

**Android:**

- Gradle wrapper JAR is automatically downloaded if missing
- Java environment is auto-detected from Android Studio installation
- For IDE support, open `build_system/android` directory in Android Studio and sync with Gradle
- Requires ANDROID_SDK and ANDROID_NDK or Android Studio installation

**iOS:**

- Requires APPLE_DEVELOPER_TEAM_ID environment variable for device deployment
- Supports both simulator and device builds
- Automatic app installation and launch on connected devices

**macOS:**

- Generates native Xcode projects with proper bundle configuration
- Supports both Debug and Release configurations

**GLFW:**

- Cross-platform build supporting Windows, macOS, and Linux
- Requires Vulkan SDK environment variable

## Architecture Overview

### Core Design Principles

- **Framework-agnostic**: Application logic is separated from platform-specific code
- **RHI abstraction**: Minimal Render Hardware Interface supporting Vulkan and Metal
- **Modern C++20**: Exception handling with stack traces, RAII resource management
- **Cross-platform first**: Mobile platforms considered from day one

### Key Components

**Application Framework (`libraries/source/application/`):**

- `AppFramework` is the main entry point - extend this class for new applications
- `NativeView` handles platform-specific windowing and input
- `RenderFramework` manages the rendering pipeline lifecycle

**Task System (`libraries/include/core/task/`):**

- `TaskManager` provides modern C++ task scheduling and parallel execution
- `TaskDispatcher` handles task distribution across worker threads
- `TaskFuture` enables asynchronous task result handling
- Supports both CPU-bound and I/O-bound task execution

**RHI Layer (`libraries/include/rhi/`):**

- Minimal abstraction over Vulkan and Metal
- `RHI` singleton provides graphics API access
- Resources use deferred deletion for safe cleanup
- Bindless rendering support for ray tracing

**Rendering Pipelines:**

- Multiple pipeline implementations: CPU path tracer, GPU path tracer, forward, deferred
- Pipeline selection via config: `cpu`, `gpu`, `forward`, `deferred`, `forward_rt`, `deferred_rt`
- Ray tracing pipelines require modern GPU with RT cores

### Platform-Specific Code

**Framework Structure:**

```text
frameworks/
├── include/     # Platform headers
└── source/      # Platform implementations
    ├── android/ # Android-specific code
    ├── apple/   # macOS/iOS shared code
    ├── glfw/    # GLFW implementation
    ├── macos/   # MacOS-specific implementation
    └── ios/     # iOS-specific extensions
```

Each platform implements `NativeView` interface for windowing and input handling.

## Shader Development

**Slang → SPIRV → Metal Pipeline:**

- Write shaders in Slang shading language (.slang files)
- Automatic SPIRV compilation and Metal conversion via spirv-cross
- Bindless resources for ray tracing shaders
- Shader hot-reload support in debug builds
- Cross-platform shader compilation

**Shader Organization:**

```text
shaders/
├── include/     # Common shader headers
├── ray_trace/   # Ray tracing compute shaders
├── screen/      # Post-processing shaders
├── standard/    # Vertex/pixel shaders
└── utilities/   # Utility compute shaders
```

## Configuration System

**Hierarchical JSON Configuration:**

1. Default: `resources/config/config.json`
2. User config (runtime generated)
3. Command-line arguments (highest priority)

**Key Settings:**

- `pipeline`: Rendering pipeline selection
- `rhi`: Graphics API (`vulkan`, `metal`)
- `max_spp`: Samples per pixel for ray tracing
- `validation`: Enable graphics API validation layers

## Development Workflow

**VSCode Integration:**

- Uses clangd with `compile_commands.json` (generated via `--clangd` flag)
- Multi-platform compile commands switching
- Debugger configurations in `.vscode/launch.json`

**Memory Debugging:**

- Address Sanitizer support via `--asan` flag
- Stack trace generation on exceptions
- Resource leak detection

**Profiling:**

- Tracy profiler integration (enabled via `--profile` flag)
- Performance markers throughout codebase
- GPU timing support

**Build Logging:**

- Detailed build logs automatically generated in `build_system/<platform>/output/build/`
- Console output with real-time progress
- Separate log files for each build attempt

## Dependencies

**External Requirements:**

- **Vulkan SDK 1.4.313.0+** (for Vulkan builds) - Required environment variable `VULKAN_SDK`
- **CMake 3.30.5+** (automatically managed by build system)
- **Ninja 1.12.1+** (automatically managed by build system)
- **GLFW3** (automatically handled by build system for GLFW platform builds)
- **Android Studio** (for Android builds - includes JBR and NDK)
  - Alternative: JDK 17+ and Android NDK separately
  - Java environment auto-detected from standard Android Studio paths
- **Xcode** (for macOS/iOS builds)
  - **macOS**: Xcode Command Line Tools minimum
  - **iOS**: Full Xcode installation required for device deployment

**Third-party Libraries (via git submodules):**

- **Eigen**: Linear algebra
- **ImGui**: Immediate mode GUI  
- **Tracy**: Profiler
- **STB**: Image loading/writing
- **TinyGLTF**: glTF model loading
- **TinyUSDZ**: USD file format support
- **nlohmann/json**: JSON parsing
- **spdlog**: Logging framework
- **magic_enum**: Enum reflection
- **mimalloc 3.x**: High-performance memory allocator
- **VMA**: Vulkan Memory Allocator
- **SPIRV-Reflect**: SPIRV reflection
- **cpptrace**: Stack trace generation
- **thread-pool**: Modern C++ thread pool
- **hash-library**: Fast hash functions
- **fast_float**: High-performance string to float conversion
- **bvh**: Bounding Volume Hierarchy
- **argparse**: Command line argument parsing
- **xoshiro_cpp**: High-quality random number generation
- **volk**: Vulkan meta-loader
- 20+ additional libraries in `thirdparty/`

All third-party dependencies are automatically managed through git submodules and require no manual installation.

**Centralized Prerequisites Management:**

- Version requirements centralized in `prerequisites.json`
- Automatic version validation and environment setup
- Consistent toolchain versions across all platforms

## Build System Architecture

The build system consists of several key components:

**Root Build Script (`build.py`):**

- Main entry point for all build operations
- Handles argument parsing and environment validation
- Orchestrates setup, configuration, and building
- Automatically runs git submodule updates and resource setup

**Platform-Specific Build Modules:**

- `build_system/glfw/build.py` - Cross-platform GLFW builds
- `build_system/macos/build.py` - macOS native application builds  
- `build_system/ios/build.py` - iOS application builds with device deployment
- `build_system/android/build.py` - Android APK builds with Gradle integration

**Prerequisites Management (`build_system/prerequisites.py`):**

- Centralized version control for external dependencies
- Automatic validation of required tool versions
- Environment setup and configuration management

**Shared Utilities (`build_system/utils.py`):**

- `run_command_with_logging()` - Executes commands with real-time logging
- `download_file()` - Downloads external resources
- `extract_zip()` - Extracts archives with permission handling

**Build Output Structure:**

```text
build_system/
├── <platform>/
│   ├── output/           # Build artifacts and logs
│   ├── project/          # IDE projects (Xcode, VS, etc.)
│   └── clangd/   # clangd configuration files
```

## Common Issues

**Environment Setup:**

- **VULKAN_SDK not set**: The build system checks for this environment variable. Install Vulkan SDK and ensure the variable points to the installation directory
- **Android builds failing**: Ensure Android Studio is installed or set JAVA_HOME to JDK 17+. Build system auto-detects common Android Studio paths
- **iOS builds requiring Team ID**: Set `APPLE_DEVELOPER_TEAM_ID` environment variable for device deployment

**Build Failures:**

- **Git submodule issues**: The build system automatically runs `git submodule update --init --recursive`, but manual cleanup may be needed if repositories are corrupted
- **Resource download failures**: Resource setup downloads external assets automatically. Check network connectivity if setup fails
- **Permission errors**: The build system handles file permissions, but manual `chmod +x` may be needed for shell scripts

**Platform-Specific Issues:**

**Android:**

- Gradle wrapper JAR is automatically downloaded on first build
- ADB commands require USB debugging enabled on connected device
- Build logs available in `build_system/android/output/build/build.log`

**iOS:**

- Device deployment requires paid Apple Developer account and proper provisioning
- Simulator builds work without Team ID
- Xcode project generated in `build_system/ios/project/`

**Cross-Platform Considerations:**

- Shader compilation differs between platforms (SPIRV vs Metal)
- File path separators handled by `FileManager` abstraction
- Platform-specific UI scaling factors
- Build artifacts isolated per platform in separate output directories

## Project Structure

```text
Sparkle/
├── libraries/          # Core engine libraries
│   ├── include/       # Header files
│   │   ├── application/  # Application framework
│   │   ├── core/        # Core utilities (logging, math, task system, etc.)
│   │   │   ├── math/    # Mathematical utilities and types
│   │   │   └── task/    # Modern C++ task system
│   │   ├── io/          # I/O operations and file management
│   │   ├── renderer/    # Rendering system
│   │   ├── rhi/         # Graphics API abstraction
│   │   └── scene/       # Scene management
│   └── source/        # Implementation files
├── frameworks/        # Platform-specific code
│   ├── android/       # Android implementation
│   ├── apple/         # macOS/iOS shared code
│   ├── glfw/          # GLFW implementation
│   ├── ios/           # iOS-specific code
│   └── macos/         # macOS-specific code
├── shaders/           # Slang shader files
│   ├── include/       # Common shader headers
│   ├── ray_trace/     # Ray tracing compute shaders
│   ├── screen/        # Post-processing shaders
│   ├── standard/      # Vertex/pixel shaders
│   └── utilities/     # Utility compute shaders
├── thirdparty/        # Third-party dependencies (git submodules)
├── build_system/      # Build configuration per platform
├── resources/         # Assets and configuration
├── prerequisites.json # Centralized version requirements
└── .github/           # CI/CD workflows
```

## Testing and Quality Assurance

**Continuous Integration:**

- GitHub Actions CI pipeline at `.github/workflows/ci.yml`
- Multi-platform builds (Windows, macOS, Linux, Android)
- All PRs required to pass CI before merging
- Built artifacts uploaded and available for download
- macOS/iOS artifacts require local building with developer account

**Code Quality:**

- **C++/Objective-C/Slang**: `.clang-format` (clang-format)
- **Markdown**: `.markdownlint.json` (markdownlint)
- **Python**: PEP8 (autopep8)
- Stack trace generation on exceptions
- Comprehensive logging with multiple levels
- Always follow the coding style guidelines. Use clang-format, markdownlint, and autopep8 to validate your formatting.
- Comment verbosity: describe structural and algorithmic design. Describe caveats and exceptions that are not immediately obvious.
- Comment verbosity: avoid commenting self-explaining code. Treat every line as a final product that goes into the repository.
- Write code that is self-explaining.
- Review code strictly as if you are Linus Torvalds.
- Remove unused code, arguments, variables, includes, etc.
- Keep design clean and readable.
- Use newest C++ features.

## Development Environment

**IDE Support:**

- **VSCode**: Primary IDE with clangd integration
- **Xcode**: Native macOS/iOS development
- **Android Studio**: Android development
- **Visual Studio**: Windows development

**Debugging and Profiling:**

- AddressSanitizer support via `--asan` flag
- Tracy profiler integration via `--profile` flag
- GPU timing and performance markers
- Memory leak detection
- Hot-reload for rapid iteration

## Target Audience

**Primary Users:**

- Graphics programming researchers and students
- Mobile rendering developers
- Developers learning advanced rendering techniques
- Cross-platform development enthusiasts

**Use Cases:**

- Educational tool for modern rendering concepts
- Research platform for mobile ray tracing
- Prototype for advanced rendering techniques
- Cross-platform development reference

## Important Notes

**Limitations:**

- **Not production-ready** - designed for education and research
- **Experimental nature** - APIs may change frequently
- **Mobile ray tracing** - requires modern hardware with RT cores
- **Limited feature set** - focused on core rendering concepts

**Requirements:**

- **Hardware**: Modern GPU with ray tracing support recommended
- **Software**: Platform-specific SDKs and development tools
- **Experience**: Intermediate to advanced C++ knowledge recommended
