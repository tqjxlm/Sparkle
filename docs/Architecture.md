# Architecture

Sparkle builds one app per platform framework. A thin framework layer hosts a shared engine library: the main thread runs the scene, a render thread renders it through one of four pipelines, and an RHI abstracts Vulkan and Metal.

## Modules

A frame flows top to bottom: the framework drives the application's main loop, which updates the scene on the main thread and hands render tasks to the render thread, where the renderer records a render graph and the RHI submits it.

| module        | role                                                                                                                   |
| ------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `frameworks`  | per-platform entry point, window and native input (glfw, macos, ios, android), behind `NativeView`                     |
| `application` | `AppFramework` main loop, `RenderFramework` render thread, `InputManager`, `SessionManager`, `TestCase`, cook pipeline |
| `scene`       | scene graph, components and materials; `SceneManager` loads scenes                                                     |
| `renderer`    | one `Renderer` per pipeline over render proxies that mirror the scene; render graph, passes, denoisers                 |
| `rhi`         | `RHIContext` over Vulkan or Metal                                                                                      |
| `io`          | image, glTF and USD loading, USD export, texture compression                                                           |
| `core`        | tasks, config, files and paths, logging, events, the cook artifact store                                               |

Subsystems have their own docs, listed in the [index](README.md#subsystems).

## Repository Map

```text
build.py, run.py          build pipeline and launcher entry points (see Build.md, Run.md)
build_system/<framework>/ per-framework builders; build output in output/, IDE projects in project/, archives in product/
libraries/include/        engine headers, one directory per layer above
libraries/source/         engine sources, plus platform/ for OS-specific code
frameworks/source/        framework layer (apple/ is shared by macos and ios)
shaders/                  Slang sources: standard/ raster, ray_trace/ path tracer, screen/ post and cook,
                          utilities/ compute helpers, include/ shared headers, nrd/ the NRD shader cook,
                          metalfx/ and fsr/ the MetalFX and FSR passes
resources/                packaged assets (packed/) and default config
tests/                    test cases, evaluators, registry.json and coverage.csv (see Test.md)
dev/                      validation, CI generator and developer tools
ide/                      IDE config templates
thirdparty/               dependencies as git submodules, plus a vendored FidelityFX subset
```

## Key Entry Points

| Component         | Location                                                                                            | Role                                        |
| ----------------- | --------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| `AppFramework`    | [libraries/source/application/](../libraries/source/application/)                                   | application base class and main loop        |
| `RenderFramework` | [libraries/source/application/](../libraries/source/application/)                                   | render thread and renderer lifecycle        |
| `NativeView`      | [frameworks/source/](../frameworks/source/)                                                         | platform window and input interface         |
| `RHIContext`      | [libraries/include/rhi/RHI.h](../libraries/include/rhi/RHI.h)                                       | per-device render context (Vulkan or Metal) |
| `TaskManager`     | [libraries/include/core/task/](../libraries/include/core/task/)                                     | thread pool and cross-thread tasks          |
| `ConfigManager`   | [libraries/include/core/ConfigManager.h](../libraries/include/core/ConfigManager.h)                 | config value registry (see Run.md)          |
| `Scene`           | [libraries/include/scene/Scene.h](../libraries/include/scene/Scene.h)                               | scene graph root; owns all components       |
| `MaterialManager` | [libraries/include/scene/material/](../libraries/include/scene/material/)                           | material lifecycle and lookup               |
| `Renderer`        | [libraries/include/renderer/renderer/](../libraries/include/renderer/renderer/)                     | base of the pipeline renderers              |
| `SessionManager`  | [libraries/include/application/SessionManager.h](../libraries/include/application/SessionManager.h) | save and load session state                 |
| `InputManager`    | [libraries/include/application/InputManager.h](../libraries/include/application/InputManager.h)     | input pipeline (see Input.md)               |

## Threading

The main thread owns the scene graph (`Scene`, `SceneNode`, components) and the render thread owns the render proxies behind it (`SceneRenderProxy`, with one proxy per renderable component and per material). A component reaches its proxy only through render tasks (`TaskManager::RunInRenderThread`), which capture raw pointers to the component and to the scene render proxy. These rules keep those pointers valid:

* **Frame batches**: render tasks queued during a main thread frame reach the render thread as one batch when the frame ends (`RenderFramework::PushRenderTasks`), in queue order. The render thread runs a batch, then renders its frame; at most one batch waits (`MaxBufferedTaskFrames`), so the main thread runs at most two frames ahead. Without a render thread of its own (`render_thread false`, a cook process, shutdown) a render task runs where it is queued.
* **Drain before destroy**: `TaskManager::FlushRenderThread` hands the render thread the tasks queued so far as a batch of their own and blocks until they ran. Whatever destroys an object that render tasks capture calls it first: `Scene::Cleanup` and `SceneNode::RemoveChild` before destroying components, and `Scene::~Scene` again before its scene render proxy dies, because the removal tasks of destroyed components capture it. A pipeline switch recreates every render proxy on the render thread, so `AppFramework::MainLoop` queues the recreation and waits the same way. `RenderFramework::StopRenderThread` hands over the remaining tasks with the stop; the render thread runs every batch without rendering, then destroys the renderer, while the scene render proxy and the RHI outlive it. Only the main thread waits: the render thread waits on the main thread for nothing but its next batch.
* **Windowed Metal drawables**: the main thread acquires a drawable at the start of each frame (`AppleViewController`), which also bounds the frames in flight, and the render thread takes them in order (`MetalView`). A render frame that finds no drawable is skipped rather than waited for, since a main thread frame that flushes hands over two batches but acquires one drawable.
* **Detached trees are inert**: a component has a render proxy and scene registrations only while attached. Loaders build their trees detached on their own thread, without touching the scene or the render thread, so dropping a tree that never attached has no effect. A camera becomes the main camera when it attaches.
* **Load generations**: `Scene::Cleanup` retires the scene's generation, which a `SceneAsyncTask` reports through `IsActive` even after the `Scene` is destroyed. `SceneManager::LoadScene` attaches what it loaded only while the generation it started in is active; a superseded load reports failure and drops its tree.
