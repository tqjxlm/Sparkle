# Sparkle

[![CI](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml/badge.svg)](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml)

Yet another cross-platform renderer with hardware ray-tracing support. Its main goal is to explore possibilities for rendering with newest hardware features on mobile devices.

It is an experimental demo which aims to be simple and modern, rather than being rich in features or robust for production.

![Failed to load image](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu.png)

## Features

### Now Implemented

* four rendering pipelines, selected with `--pipeline`: `cpu` and `gpu` path tracers (the gpu one on ray-tracing hardware, with bindless resources and NRD or MetalFX denoising), and `forward` and `deferred` rasterizers
* a render graph that derives barriers, render pass merging and tile-memory use from declared pass accesses
* RHI layer supporting Vulkan and Metal
* shader cross compilation from Slang
* platform-specific wrappers for Windows, Linux, Android, MacOS and iOS
* fully-scripted build, cook and package pipeline based on CMake
* build-time asset cooking with block-compressed textures and HDR cube maps
* data loaders for images(stb), meshes(glTF) and scenes(USD), and USD export
* native file/asset management
* stacktrace support (based on cpptrace)
* logging system (based on spdlog)
* config system (based on argparse and json)
* ui system (based on imgui)
* profiling system (based on tracy)

### Not in the Near Future

* editor
* compatibility for older hardware, systems or compilers
* gameplay (scripting, audio, network, physics, etc..)

### Framework Support

| framework | supported platforms   | supported generators           |
| --------- | --------------------- | ------------------------------ |
| glfw      | windows, macos, linux | ninja, makefile, Visual Studio |
| android   | android               | Android Studio                 |
| macos     | macos                 | Xcode                          |
| ios       | ios                   | Xcode                          |

### Graphics API Support

| pipeline         | windows     | linux       | macos                    | ios       | android        |
| ---------------- | ----------- | ----------- | ------------------------ | --------- | -------------- |
| cpu              | vulkan-glfw | vulkan-glfw | metal-macos, vulkan-glfw | metal-ios | vulkan-android |
| gpu              | vulkan-glfw | vulkan-glfw | metal-macos              | metal-ios | vulkan-android |
| forward/deferred | vulkan-glfw | vulkan-glfw | metal-macos, vulkan-glfw | metal-ios | vulkan-android |

The Vulkan backend requires a Vulkan 1.3 device.

### Tested on Devices

| platform | windows          | android          | macos        | ios           |
| -------- | ---------------- | ---------------- | ------------ | ------------- |
| system   | windows 11       | android 13       | macos 26.2   | ios 26.2      |
| cpu      | Ryzen 5975WX     | Snapdragon 8Gen2 | Apple M3 Pro | Apple A18 Pro |
| gpu      | GeForce RTX 4080 | Adreno 740       | Apple M3 Pro | Apple A18 Pro |

## Try it Now

You can try the latest builds on [Github releases](https://github.com/tqjxlm/Sparkle/releases). See [docs/Run.md](docs/Run.md) for how to use the app.

## Documentation

[docs/README.md](docs/README.md) indexes the docs: building, running, testing, architecture and subsystem design. Contributions follow [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md); planned work and known issues are in [docs/TODO.md](docs/TODO.md).
