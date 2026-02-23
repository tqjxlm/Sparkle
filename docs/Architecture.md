# Architecture

## Key Entry Points

| Component         | Location                                                                                         | Description                                 |
| ----------------- | ------------------------------------------------------------------------------------------------ | ------------------------------------------- |
| `AppFramework`    | [libraries/source/application/](libraries/source/application/)                                   | Main application base class                 |
| `RenderFramework` | [libraries/source/application/](libraries/source/application/)                                   | Rendering pipeline lifecycle                |
| `NativeView`      | [frameworks/source/](frameworks/source/)                                                         | Platform windowing/input interface          |
| `RHI`             | [libraries/include/rhi/](libraries/include/rhi/)                                                 | Graphics API abstraction singleton          |
| `TaskManager`     | [libraries/include/core/task/](libraries/include/core/task/)                                     | Async task scheduling                       |
| `Scene`           | [libraries/source/scene/](libraries/source/scene/)                                               | Scene graph root; owns all components       |
| `Renderer`        | [libraries/include/renderer/](libraries/include/renderer/)                                       | Abstract base for all pipeline renderers    |
| `RHIContext`      | [libraries/include/rhi/RHI.h](libraries/include/rhi/RHI.h)                                       | Per-device render context (Vulkan or Metal) |
| `ConfigManager`   | [libraries/include/core/ConfigManager.h](libraries/include/core/ConfigManager.h)                 | Config system registry (cvar registration)  |
| `MaterialManager` | [libraries/include/scene/material/](libraries/include/scene/material/)                           | Material lifecycle and lookup               |
| `SessionManager`  | [libraries/include/application/SessionManager.h](libraries/include/application/SessionManager.h) | Save/load session state                     |

## Build System Architecture

The build system uses a factory pattern with abstract `FrameworkBuilder` interface:

* [build.py](build.py) - Main entry point, orchestrates setup and build
* [build_system/builder_interface.py](build_system/builder_interface.py) - Abstract `FrameworkBuilder` base class
* [build_system/builder_factory.py](build_system/builder_factory.py) - Creates platform-specific builders
* [build_system/prerequisites.py](build_system/prerequisites.py) - Auto-installs CMake, Ninja, Vulkan SDK
* Platform builders: `glfw/build.py`, `macos/build.py`, `ios/build.py`, `android/build.py`

**Build output:** `build_system/<platform>/output/` (artifacts), `project/` (IDE), `product/` (archives)

## Shader Architecture

**Pipeline:** Slang (.slang) → SPIRV → Metal (via spirv-cross)

```text
shaders/
├── include/      # Common headers
├── ray_trace/    # RT compute shaders
├── screen/       # Post-processing
├── standard/     # Vertex/pixel shaders
└── utilities/    # Utility compute
```

## Repository Structure

```text
sparkle/
├── build.py              # Main build entry point
├── build_system/         # Per-platform build scripts and output artifacts
│   └── <platform>/       # glfw, macos, ios, android — build output here
├── libraries/            # Core engine libraries
│   ├── include/          # Public headers (consumed by all targets)
│   │   ├── application/  # App lifecycle (AppFramework, RenderFramework, NativeView…)
│   │   ├── core/         # Utilities (TaskManager, ConfigManager, FileManager…)
│   │   ├── rhi/          # Graphics API abstraction (RHIContext, RHI…)
│   │   ├── renderer/     # Render passes and scene render proxies
│   │   ├── scene/        # Scene graph (components, materials)
│   │   └── io/           # Data loaders (glTF, USD, images)
│   └── source/           # Implementations
├── frameworks/source/    # Platform wrappers (glfw, macos, ios, android)
├── shaders/              # Slang shader source
│   ├── ray_trace/        # Path tracer compute shaders
│   ├── screen/           # Post-processing shaders
│   ├── standard/         # Vertex/pixel shaders
│   └── utilities/        # Utility compute shaders
├── resources/            # Assets: models, textures, default config
├── dev/                  # Developer scripts (functional_test.py…)
├── ide/                  # IDE config templates (.vscode, launch configs)
└── thirdparty/           # Third-party dependencies (git submodules)
```
