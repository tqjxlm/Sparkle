# Architecture

## Key Entry Points

| Component         | Location                                                                                            | Description                                    |
| ----------------- | --------------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| `AppFramework`    | [libraries/source/application/](../libraries/source/application/)                                   | Main application base class                    |
| `RenderFramework` | [libraries/source/application/](../libraries/source/application/)                                   | Rendering pipeline lifecycle                   |
| `NativeView`      | [frameworks/source/](../frameworks/source/)                                                         | Platform windowing/input interface             |
| `RHI`             | [libraries/include/rhi/](../libraries/include/rhi/)                                                 | Graphics API abstraction singleton             |
| `TaskManager`     | [libraries/include/core/task/](../libraries/include/core/task/)                                     | Async task scheduling                          |
| `Scene`           | [libraries/source/scene/](../libraries/source/scene/)                                               | Scene graph root; owns all components          |
| `Renderer`        | [libraries/include/renderer/](../libraries/include/renderer/)                                       | Abstract base for all pipeline renderers       |
| `RHIContext`      | [libraries/include/rhi/RHI.h](../libraries/include/rhi/RHI.h)                                       | Per-device render context (Vulkan or Metal)    |
| `ConfigManager`   | [libraries/include/core/ConfigManager.h](../libraries/include/core/ConfigManager.h)                 | Config system registry (cvar registration)     |
| `MaterialManager` | [libraries/include/scene/material/](../libraries/include/scene/material/)                           | Material lifecycle and lookup                  |
| `SessionManager`  | [libraries/include/application/SessionManager.h](../libraries/include/application/SessionManager.h) | Save/load session state                        |
| `InputManager`    | [libraries/include/application/InputManager.h](../libraries/include/application/InputManager.h)     | Input event pipeline, see [Input.md](Input.md) |

## Scene And Render Thread

The main thread works on the scene graph (`Scene`, `SceneNode`, components) and the render thread on the render proxies behind it (`SceneRenderProxy`, with one proxy per renderable component and per material). A component reaches its proxy only through render tasks (`TaskManager::RunInRenderThread`), which capture raw pointers to the component and to the scene render proxy.

* **Frame batches**: render tasks queued during a main thread frame reach the render thread as one batch when the frame ends (`RenderFramework::PushRenderTasks`), in the order they were queued. The render thread runs a batch, then renders its frame; at most one batch waits (`MaxBufferedTaskFrames`), so the main thread runs at most two frames ahead. In a process without a render thread of its own (`render_thread false`, a cook process, shutdown) a render task runs where it is queued.
* **Drain before destroy**: `TaskManager::FlushRenderThread` hands the render thread the tasks queued so far as a batch of their own and blocks the main thread until they ran. Whatever destroys an object that render tasks capture calls it first, so no caller has to: `Scene::Cleanup` and `SceneNode::RemoveChild` before they destroy components, and `Scene::~Scene` once more before its scene render proxy dies, because the removal tasks of the destroyed components capture it. A pipeline switch recreates every render proxy by walking the scene graph on the render thread, so `AppFramework::MainLoop` queues the recreation and waits for it the same way. Only the main thread waits this way: the render thread waits on the main thread for nothing but its next batch.
* **Detached trees are inert**: a component has a render proxy and its registrations in the scene only while it is attached. Loaders build their trees detached, on their own thread, without reading the scene or touching the render thread, so dropping a tree that never attached leaves both alone. A camera becomes the main camera when it attaches.
* **Load generations**: `Scene::Cleanup` retires the scene's generation, which a `SceneAsyncTask` reports through `IsActive` even after the `Scene` is destroyed. `SceneManager::LoadScene` attaches what it loaded, and the defaults the file lacks, only while the generation it started in is active; a load that was superseded reports failure and its tree is dropped.

## Build System Architecture

The build system uses a factory pattern with abstract `FrameworkBuilder` interface:

* [build.py](../build.py) - Main entry point, orchestrates setup and build
* [build_system/builder_interface.py](../build_system/builder_interface.py) - Abstract `FrameworkBuilder` base class
* [build_system/builder_factory.py](../build_system/builder_factory.py) - Creates platform-specific builders
* [build_system/prerequisites.py](../build_system/prerequisites.py) - Auto-installs CMake, Ninja, Vulkan SDK
* Platform builders: `glfw/build.py`, `macos/build.py`, `ios/build.py`, `android/build.py`

**Build output:** `build_system/<platform>/output/` (artifacts), `project/` (IDE), `product/` (archives)

## Shader Architecture

**Pipeline:** Slang (`.slang`) → SPIR-V for Vulkan; Metal is emitted directly from Slang, except `ray_trace/` shaders which go SPIR-V → MSL via spirv-cross (see [shaders/CMakeLists.txt](../shaders/CMakeLists.txt)).

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
│   ├── include/          # Shared shader headers
│   ├── nrd/              # NRD shader cook (see Denoiser.md)
│   ├── ray_trace/        # Path tracer compute shaders
│   ├── screen/           # Post-processing shaders
│   ├── standard/         # Vertex/pixel shaders
│   └── utilities/        # Utility compute shaders
├── resources/            # Assets: models, textures, default config
├── dev/                  # Developer entry points and validation orchestrator
├── tests/                # Focused test cases, evaluators, and test helpers
├── ide/                  # IDE config templates (.vscode, launch configs)
└── thirdparty/           # Third-party dependencies (git submodules)
```
