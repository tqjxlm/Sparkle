# Build

`build.py` turns the source tree into a distributable in three stages: build, cook and package. It never launches the app; `run.py` does (see [Run.md](Run.md)). Along the way it installs the prerequisites it can, updates submodules, downloads sample resources, copies VS Code settings and wires the git hooks.

## Quick Start

``` shell
# GLFW Windows release build with clang-cl
$env:VULKAN_SDK='D:/SDKs/VulkanSDK/1.4.363.0'
$env:VCPKG_PATH='D:/SDKs/vcpkg'
python3 build.py --framework glfw --config Release

# GLFW macOS debug build with llvm
export VULKAN_SDK=/Users/username/VulkanSDK/1.4.363.0
python3 build.py --framework glfw

# native macOS app through Xcode, debug
python3 build.py --framework macos

# iOS Xcode project without building
export APPLE_DEVELOPER_TEAM_ID=ABC123DEF4
python3 build.py --framework ios --apple_auto_sign --generate_only
```

## Prerequisites

### Toolchains

| framework                                | windows                 | macos              | linux           | ios                | android               |
| ---------------------------------------- | ----------------------- | ------------------ | --------------- | ------------------ | --------------------- |
| glfw                                     | clang-cl (vs-installer) | apple-llvm (Xcode) | clang 19+ (apt) | -                  | -                     |
| glfw (VS solution via `--generate_only`) | msvc (Visual Studio)    | -                  | -               | -                  | -                     |
| macos                                    | -                       | apple-llvm (Xcode) | -               | -                  | -                     |
| ios                                      | -                       | -                  | -               | apple-llvm (Xcode) | -                     |
| android                                  | -                       | -                  | -               | -                  | llvm (Android Studio) |

The windows glfw build compiles with the clang-cl bundled in Visual Studio (`VC/Tools/Llvm`), never another clang-cl on `PATH`, because the MSVC STL accepts only the clang version it ships with (`error STL1000: Unexpected compiler version`).

The android build needs Android Studio Quail 4 or newer and JDK 17 or newer (Android Gradle Plugin 9.4).

### Installed Dependencies

* **Vulkan SDK** 1.4.363.0+ from <https://vulkan.lunarg.com/sdk/home>: the Vulkan loader (and MoltenVK on macOS) that glfw builds link, the validation layer and shader tools.
* **GLFW**, for the glfw framework only:

  ``` shell
  brew install glfw # MacOS
  vcpkg install glfw3:x64-windows # Windows
  sudo apt install libglfw3-dev # Linux
  ```

* **CMake** 3.30+: `brew install cmake` on macOS, <https://cmake.org/download/> on Windows.
* **ccache** (optional): when it is on `PATH`, every framework compiles through it, so rebuilding code that was built before reuses cached objects.

  ``` shell
  brew install ccache # MacOS
  choco install ccache # Windows
  sudo apt install ccache # Linux
  ```

The build finds prerequisites on its own and installs the ones it can. Point it at the rest with environment variables:

| variable     | example                                                      | valid for        | auto-installable                |
| ------------ | ------------------------------------------------------------ | ---------------- | ------------------------------- |
| VULKAN_SDK   | /Users/username/VulkanSDK/1.4.363.0                          | all              | yes (requires admin on windows) |
| CMAKE_PATH   | /opt/homebrew/bin/cmake                                      | all              | yes                             |
| VCPKG_PATH   | D:/SDKs/vcpkg                                                | all windows      | yes                             |
| LLVM         | /opt/homebrew/opt/llvm                                       | non-windows glfw | yes                             |
| ANDROID_HOME | /Users/username/AndroidSDK                                   | android          | no (Android Studio)             |
| JAVA_HOME    | /Applications/Android Studio.app/Contents/jbr/Contents/Home  | android          | no (Android Studio)             |
| VS_PATH      | C:/Program Files/Microsoft Visual Studio/[version]/[edition] | all windows      | no (vs-installer)               |
| ISPC         | /Users/username/ispc/bin/ispc                                | glfw, macos      | yes                             |

### Pinned Tools

[prerequisites.json](../prerequisites.json) pins the versions an installed SDK must not change:

* **Vulkan headers**: every Vulkan framework compiles against the [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers.git) submodule at the SDK version, never the SDK's or NDK's headers. Bump both together.
* **slangc**: installed into `build_cache`, replacing any other version there. A version change recompiles every shader.
* **Android CMake**: the android native build runs the SDK's `cmake;<version>` package at the pinned version, installed through `sdkmanager` when missing, with the pinned ninja (the package's own ninja cannot build C++20 module sources).
* **ISPC** compiles the cook's block encoder kernels, only for the frameworks that can cook (glfw, macos).

<details>

<summary>Bundled dependencies, set up as git submodules or by CMake. Many thanks to them!</summary>

* [argparse](https://github.com/p-ranav/argparse.git)
* [bvh](https://github.com/madmann91/bvh.git)
* [cpptrace](https://github.com/jeremy-rifkin/cpptrace.git)
* [eigen](https://gitlab.com/libeigen/eigen.git)
* [fast_float](https://github.com/fastfloat/fast_float.git) (the copy bundled with tinyusdz)
* [hash-library](https://github.com/lazy-eggplant/hash-library.git)
* [imgui](https://github.com/ocornut/imgui.git) and [imgui_club](https://github.com/ocornut/imgui_club.git)
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
* [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers.git)
* [Xoshiro-cpp](https://github.com/Reputeless/Xoshiro-cpp.git)

</details>

## Options

`python3 build.py --help` lists every option. The common ones:

* `--framework=<glfw|macos|ios|android>`: what to build. `glfw` targets the host's desktop OS.
* `--config=Release`: Release build (default: Debug).
* `--stage=<build|cook|package|all>`: pipeline stages to run, repeatable (see [Pipeline Stages](#pipeline-stages)).
* `--cook_targets=<a+b+...>`: cook targets to produce content images for (default: the built framework's own). Known targets: `android`, `ios`, `macos`, `macos-glfw`, `windows-glfw`, `linux-glfw`.
* `--generate_only`: generate the IDE project without building. `--configure_only` runs the configure step only.
* `--clangd`: generate `compile_commands.json` for clangd.
* `--clean`: clean the output directory before configuring, which resolves stale configure state.
* `--profile`: enable the Tracy client. Connect with the [Tracy](https://github.com/wolfpld/tracy/releases) GUI of the same release as `thirdparty/tracy`; other releases refuse the connection.
* `--asan`: enable AddressSanitizer.
* `--strip_test`: compile out TestCase support (see [Test.md](Test.md)).
* `--apple_auto_sign`: automatic code signing for Apple platforms; needs `APPLE_DEVELOPER_TEAM_ID` ([how to find it](https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/)).
* `--ios_platform=<device|simulator>`: `simulator` builds an unsigned package for the host's iOS Simulator; switching resets the iOS project and output trees.
* `--android_abi=<arm64-v8a|x86_64>`: `x86_64` builds for emulators on x86 hosts.
* `--cmake-args='...'`: extra CMake arguments, e.g. `-DENABLE_LTO=ON` for link time optimization (off by default: it adds minutes of Release link time).

`SPARKLE_BUILD_JOBS` overrides the glfw build parallelism (default 64 on windows, 16 elsewhere).

## Pipeline Stages

The stages always run in the order build → cook → package. A plain `build.py` runs all of them and produces a complete distributable: the built app, cooked content and a product archive in `build_system/<framework>/product/`. `run.py` runs only the stages a launch consumes.

* Each stage is idempotent through its own cache and fails fast when an earlier stage's output is missing; it never runs an upstream stage implicitly. Explicit selections, including `--stage all`, never degrade.
* **Cook** runs the cooker inside the host framework's binary (`macos` on macOS, `glfw` elsewhere) whatever the target, and fails with a hint when that binary is not built. It writes one content image per cook target; see [Cooking.md](Cooking.md#content-images-and-packaging). Its log goes to `logs/cook.log` under the external log directory.
* **Package** replaces the archive's assets with the target's content image and re-signs packages whose signature the rewrite breaks. A package without cooked content is an error.
* Every binary embeds the git commit and branch it was built from ([cmake/GitVersion.cmake](../cmake/GitVersion.cmake)) and logs them at launch, which is how to tell whether a binary, e.g. the host cooker, is stale.

The host and the target framework are independent, within toolchain limits:

| target \ host | macOS | Windows | Linux |
| ------------- | ----- | ------- | ----- |
| `glfw`        | ✔     | ✔       | ✔     |
| `macos`       | ✔     | ✗       | ✗     |
| `ios`         | ✔     | ✗       | ✗     |
| `android`     | ✔     | ✔       | ✔     |

`macos` and `ios` need Xcode. `glfw` has no cross-OS build, so its archives carry the host system in their name (`windows-glfw-Release.zip`); other archives are named by target (`android-Release.apk`, `macos-Release.zip`).

## IDE

### Visual Studio Code

The first build copies [ide/.vscode/settings.json](../ide/.vscode/settings.json) and [launch.json](../ide/.vscode/launch.json) to `.vscode/` unless they exist.

* **Intellisense**: run `python3 build.py --framework=<framework> --clangd`, install the clangd extension, adjust the tool paths in `.vscode/settings.json` and keep the `--compile-commands-dir` line of your framework.
* **Debugging**: install the CodeLLDB extension and adjust the paths in `.vscode/launch.json` (see the [launch.json reference](https://code.visualstudio.com/docs/cpp/launch-json-reference)).

### Visual Studio, Rider, CLion (glfw on Windows)

``` shell
python3 build.py --framework=glfw --generate_only
start build_system/glfw/project/sparkle.sln   # start-up project: sparkle
```

### Xcode (macos and ios)

``` shell
python3 build.py --framework=macos --generate_only    # or ios
open build_system/macos/project/sparkle.xcodeproj     # start-up target: sparkle
```

### Android Studio

Run `python3 build.py --framework=android --generate_only`, open `build_system/android` and pick the build variant in the build configuration panel.

## Build System Internals

* [build.py](../build.py) orchestrates setup and the stages; [build_system/builder_factory.py](../build_system/builder_factory.py) creates the framework's `FrameworkBuilder` ([builder_interface.py](../build_system/builder_interface.py)), implemented in `build_system/<framework>/build.py`.
* [build_system/prerequisites.py](../build_system/prerequisites.py) finds and installs CMake, Ninja, the Vulkan SDK and the pinned tools.
* Shaders compile from Slang to SPIR-V for Vulkan and directly to MSL for Metal, except `ray_trace/` shaders, which go through SPIR-V and spirv-cross ([shaders/CMakeLists.txt](../shaders/CMakeLists.txt)). A shader error on Metal can therefore come from either step's log.
