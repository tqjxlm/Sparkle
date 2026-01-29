# Sparkle

[![CI](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml/badge.svg)](https://github.com/tqjxlm/Sparkle/actions/workflows/ci.yml)

Yet another cross-platform renderer with hardware ray-tracing support. Its main goal is to explore possibilities for rendering with newest hardware features on mobile devices.

It is an experimental demo which aims to be simple and modern, rather than being rich in features or robust for production.

![Failed to load image](https://ooo.0x0.ooo/2025/06/24/OVgDd1.jpg)

## Features

### Now implemented

* CPU path tracer
* GPU path tracer with bindless
* rasterization-based renderers (forward and deferred)
* RHI layer supporting Vulkan and Metal
* shader cross compilation
* platform-specific wrappers for Windows, Android, MacOS and iOS
* fully-scripted building system based on CMake
* data loaders for images(stb), meshes(glTF) and scenes(USD)
* stacktrace support
* logging system
* config system
* ui system
* profiling system
* native file/asset management

### Not in the near future

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

## Dependencies

### Compiler

| framework | windows                 | macos              | ios                | android               |
| --------- | ----------------------- | ------------------ | ------------------ | --------------------- |
| glfw      | clang-cl (vs-installer) | apple-llvm (Xcode) | -                  | -                     |
| glfw-sln  | msvc (Visual Studio)    | -                  | -                  | -                     |
| macos     | -                       | apple-llvm (Xcode) | -                  | -                     |
| ios       | -                       | -                  | apple-llvm (Xcode) | -                     |
| android   | -                       | -                  | -                  | llvm (Android Studio) |

### External Dependencies

These libraries should be installed via an installer or package manager (apt, brew, vcpkg).

* **Package Manager**

  ``` shell
  homebrew # MacOS
  vcpkg # Windows
  apt # Linux
  ```

* **Vulkan SDK**: 1.4.313.0+

  ``` shell
  https://vulkan.lunarg.com/sdk/home
  ```

* **GLFW** (required for glfw framework only)

  ``` shell
  brew install glfw # MacOS
  vcpkg install glfw3:x64-windows # Windows
  ```

* **CMake**: 3.24+

  ``` shell
  brew install cmake # MacOS
  https://cmake.org/download/ # Windows
  ```

### Internal Dependencies

These libraries are managed by git submodules or CMake. They will be setup automatically when you run the build script. (see build.py)

<details>

<summary>Click to expand the list of internal dependencies. Many thanks to them!</summary>

* [argparse](https://github.com/p-ranav/argparse.git)
* [bvh](https://github.com/madmann91/bvh.git)
* [cpptrace](https://github.com/jeremy-rifkin/cpptrace.git)
* [eigen](https://gitlab.com/libeigen/eigen.git)
* [fast_float](https://github.com/fastfloat/fast_float.git)
* [hash-library](https://github.com/lazy-eggplant/hash-library.git)
* [imgui](https://github.com/ocornut/imgui.git)
* [ios-cmake](https://github.com/leetal/ios-cmake.git)
* [json](https://github.com/nlohmann/json.git)
* [magic_enum](https://github.com/Neargye/magic_enum.git)
* [mimalloc](https://github.com/microsoft/mimalloc.git)
* [spdlog](https://github.com/gabime/spdlog.git)
* [spirv_reflect](https://github.com/KhronosGroup/SPIRV-Reflect.git)
* [stb](https://github.com/nothings/stb.git)
* [thread-pool](https://github.com/bshoshany/thread-pool.git)
* [tinygltf](https://github.com/syoyo/tinygltf.git)
* [tinyusdz](https://github.com/lighttransport/tinyusdz.git)
* [tracy](https://github.com/wolfpld/tracy.git)
* [vma](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
* [volk](https://github.com/zeux/volk.git)
* [Xoshiro-cpp](https://github.com/Reputeless/Xoshiro-cpp.git)

</details>

### Resources

Sample resources will be automatically setup during the first build. (see build.py)

## Build & Run

Before building, make sure you have installed all dependencies.

The build system will try to find prerequisites automatically.

If it fails, it will try to install when possible.

Otherwise,you need to specify them via environment variables. See the table below for details.

| variable     | example                                                     | valid for        | auto-installable                |
| ------------ | ----------------------------------------------------------- | ---------------- | ------------------------------- |
| VULKAN_SDK   | /Users/username/VulkanSDK/1.4.313.0                         | all              | yes (requires admin on windows) |
| CMAKE_PATH   | /opt/homebrew/bin/cmake                                     | all              | yes                             |
| VCPKG_PATH   | D:/SDKs/vcpkg                                               | all windows      | yes                             |
| LLVM         | /opt/homebrew/opt/llvm                                      | non-windows glfw | yes                             |
| ANDROID_HOME | /Users/username/AndroidSDK                                  | android          | no (Android Studio)             |
| JAVA_HOME    | /Applications/Android Studio.app/Contents/jbr/Contents/Home | android          | no (Android Studio)             |
| VS_PATH      | C:/Program Files/Microsoft Visual Studio/2022/Community     | all windows      | no (vs-installer)               |

### Quick Start Examples

``` shell
# make a GLFW Windows release build with clang-cl toolchain
$env:VULKAN_SDK='D:/SDKs/VulkanSDK/1.4.313.0'
$env:VCPKG_PATH='D:/SDKs/vcpkg'
python3 build.py --framework glfw --config Release
```

``` shell
# make a GLFW MacOS debug build with llvm toolchain
export VULKAN_SDK=D:/SDKs/VulkanSDK/1.4.313.0
python3 build.py --framework glfw
```

``` shell
# generate GLFW Visual Studio solution with msvc toolchain, without building
$env:VULKAN_SDK='D:/SDKs/VulkanSDK/1.4.313.0'
$env:VCPKG_PATH='D:/SDKs/vcpkg'
python3 build.py --framework glfw --generate_only
```

``` shell
# make an Android APK debug apk and run on a connected device
$env:VULKAN_SDK='D:/SDKs/VulkanSDK/1.4.313.0'
$env:ANDROID_HOME='D:/SDKs/AndroidSDK'
python3 build.py --framework android --run
```

``` shell
# generate a MacOS Xcode project and build in debug mode
export VULKAN_SDK=/Users/username/VulkanSDK/1.4.313.0
python3 build.py --framework macos
```

``` shell
# generate an iOS Xcode project wihtout building
export VULKAN_SDK=/Users/username/VulkanSDK/1.4.313.0
export APPLE_DEVELOPER_TEAM=ABC123DEF4
python3 build.py --framework ios --apple_auto_sign --generate_only
```

``` shell
# make an iOS release build and run on a connected device in ray tracing mode
export VULKAN_SDK=/Users/username/VulkanSDK/1.4.313.0
export APPLE_DEVELOPER_TEAM=ABC123DEF4
python3 build.py --framework ios --apple_auto_sign --run --pipeline gpu
```

### Build via Script

**All platforms:**

``` shell
python3 build.py --framework=<framework> [build-options] [run-options]
```

**Supported frameworks:**

* `glfw` - Cross-platform GLFW build (Windows/MacOS)
* `android` - Android APK with automatic environment setup
* `macos` - Native MacOS application
* `ios` - iOS application

**Common build options:**

* `--config=Release` - Release build (default: Debug).
* `--archive` - Archive the app for distribution. Not required if you test the build locally. The archived app is located in `build_system/<framework>/product`.
* `--generate_only` - Generate IDE project files without building.
* `--clangd` - Generate compile_commands.json for clangd intellisense support.
* `--profile` - Enable Tracy profiler.
* `--asan` - Enable AddressSanitizer.
* `--clean` - Clean output directory before configure, which resolves some build errors.
* `--apple_auto_sign` - Enable automatic code signing for Apple platforms. Requires APPLE_DEVELOPER_TEAM_ID to be set. See [this page](https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/)
* `--help` - Show all usage help.

**Common run options:**

* `--run` - Run after building. For mobile builds, it tries to run on a connected device.
* `--pipeline` - Rendering pipeline to use (cpu, gpu, forward, deferred).
* `--scene` - Scene to render. Empty for the standard testing scene. Other values for models under resources/models.
* `--validation` - Enable graphics API validation.
* `--max-spp` - Max sample per pixel.
* `--thread` - Num threads to use for cpu pipeline.
* `--help` - Show all usage help.

### Configs

#### How to use config

* Default config: copied from resources/config/config.json to final package on every build.
* User config: generated at [InternalStoragePath]/generated/config/config.json on first run of the built package. It overrides the default config.
* Commandline arguments: given in command line if available. It overrides all config files. example:

#### Important configs

* pipeline: rendering pipeline to use (cpu, gpu, forward, forward_rt)
* scene: scene to render. empty for the standard testing scene. other values for models under resources/models.
* validation: enable graphics API validation.
* max-spp: max sample per pixel
* thread: num threads to use for cpu pipeline

Search across the project for keyword "ConfigValue" for more available configs.

## Work with IDE

Sometimes you want better debugging or intellisense support from IDEs. Follow the instructions below to generate IDE project files.

### Visual Studio Code (All platforms)

This project is configured to work with VSCode perfectly (I use it heavily when developing this project). Follow the steps to set up.

#### Intellisense

1. **Install toolchain**: For MacOS and iOS, install Xcode. For Windows, install clang-cl via Visual Studio Installer. For Android, install Android Studio.
2. **Generate project configuration**:

   ``` shell
   # a compile_commands.json file will be generated under the project build directory.
   python3 build.py --framework=<platform> --clangd
   ```

3. **Install VSCode extensions**: C/C++, clangd.
4. **Copy IDE config**: Copy `ide/.vscode/settings.json` to `.vscode/settings.json` in project root.
5. **Configure paths**: Modify paths in `.vscode/settings.json` to match your environment.
6. **Choose compile commands**: Select one of the "--compile-commands-dir" options and comment out others.
7. **Reload VSCode**: Reload window for full intellisense support.

#### Debugging

1. **Install toolchain**: For MacOS and iOS, install Xcode. For Windows, install clang-cl via Visual Studio Installer. For Android, Android Studio is required.
2. **Install VSCode extensions**: CodeLLDB.
3. **Copy IDE config**: Copy `ide/.vscode/launch.json` to `.vscode/launch.json` in project root.
4. **Configure paths**: Modify paths in `.vscode/launch.json` to match your environment.
5. **Start debugging session**: Follow [vscode documentation](<https://code.visualstudio.com/docs/cpp/launch-json-reference>)

### Visual Studio / Rider / Clion (Only for glfw framework on Windows)

``` shell
python3 build.py --framework=glfw --generate_only
start build_system/glfw/project/sparkle.sln
# select sparkle as start up project
```

### Xcode (Only for macos/ios frameworks on MacOS)

``` shell
python3 build.py --framework=macos --generate_only     # or --framework=ios
open build_system/macos/project/sparkle.xcodeproj      # or build_system/ios/project/sparkle.xcodeproj
# select sparkle as start up target
```

### Android Studio (Only for android framework)

``` shell
python3 build.py --framework=android --generate_only
# open project folder `build_system/android` in Android Studio
# select debug or release build in build configuration panel
```

## Todo list

### CI/CD

* [ ] format and style check pipeline
* [ ] unit test pipeline
* [ ] functional test pipeline
* [ ] performance test pipeline

### Path Tracing Renderers

* [ ] ASVGF
* [ ] vendor-specific denoising and super sampling
* [ ] dynamic scene

### Rasterization Renderers

* [ ] CSM
* [ ] PCSS
* [ ] AO
* [ ] ray traced shadow
* [ ] ray traced AO
* [ ] ray traced reflection

### RHI

* [ ] msaa
* [ ] render target pooling
* [ ] render graph
* [ ] subpass

### IO

* [ ] full USD support (current one is far from complete)
* [ ] external data loader interface

### Cook

* [ ] standalone cooker
* [ ] standalone shader compiler
* [ ] texture compression
* [ ] slang support for metal

### Build

* [ ] Linux support

### Infrastructure

* [ ] modularize core libraries
* [ ] rhi thread
* [ ] event based input handling

## Known Issues

* [ ] ImGui scrolling not working on android and ios
