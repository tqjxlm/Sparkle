# Sparkle

[![CI](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml/badge.svg)](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml)

Yet another cross-platform renderer with hardware ray-tracing support. Its main goal is to explore possibilities for rendering with newest hardware features on mobile devices.

It is an experimental demo which aims to be simple and modern, rather than being rich in features or robust for production.

![Failed to load image](https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle/TestScene_gpu_glfw.png)

## Features

### Now Implemented

* CPU path tracer
* GPU path tracer with bindless
* rasterization-based renderers (forward and deferred)
* RHI layer supporting Vulkan and Metal
* shader cross compilation
* platform-specific wrappers for Windows, Android, MacOS and iOS
* fully-scripted building system based on CMake
* data loaders for images(stb), meshes(glTF) and scenes(USD)
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

### Rendering Pipeline Support

| pipeline | feature                                     |
| -------- | ------------------------------------------- |
| cpu      | Path tracer on CPU                          |
| gpu      | Path tracer on raytracing hardware          |
| forward  | Forward pipeline on rasterization hardware  |
| deferred | Deferred pipeline on rasterization hardware |

### Framework Support

| framework | supported platforms | supported generators           |
| --------- | ------------------- | ------------------------------ |
| glfw      | windows, macos      | ninja, makefile, Visual Studio |
| android   | android             | Android Studio                 |
| macos     | macos               | Xcode                          |
| ios       | ios                 | Xcode                          |

### Graphics API Support

| pipeline         | windows     | macos                    | ios       | android        |
| ---------------- | ----------- | ------------------------ | --------- | -------------- |
| cpu              | vulkan-glfw | metal-macos, vulkan-glfw | metal-ios | vulkan-android |
| gpu              | vulkan-glfw | metal-macos              | metal-ios | vulkan-android |
| forward/deferred | vulkan-glfw | metal-macos, vulkan-glfw | metal-ios | vulkan-android |

### Tested on Devices

| platform | windows          | android          | macos        | ios           |
| -------- | ---------------- | ---------------- | ------------ | ------------- |
| system   | windows 11       | android 13       | macos 26.2   | ios 26.2      |
| cpu      | Ryzen 5975WX     | Snapdragon 8Gen2 | Apple M3 Pro | Apple A18 Pro |
| gpu      | GeForce RTX 4080 | Adreno 740       | Apple M3 Pro | Apple A18 Pro |

## Try it Now

You can try the latest builds on [Github releases](https://github.com/tqjxlm/Sparkle/releases).

## Development Guide

For how to build and run the project, please refer to [docs/Development.md](docs/Development.md).

## Todo List & Known Issues

For todo list and known issues, please refer to [docs/TODO.md](docs/TODO.md).

## Contributing

For how to contribute to the project, please refer to [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).
