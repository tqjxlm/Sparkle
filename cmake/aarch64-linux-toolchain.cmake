# Cross-compile the linux product to arm64 from an x86 host.
#
# The linux product is arm64 because its test cell is a jetson, but nothing that runs
# *during* the build has an aarch64 linux binary to run: LunarG publishes no aarch64
# Vulkan SDK, and DXC — which NRD needs to compile its HLSL to SPIR-V — has no official
# aarch64 linux release either. Cross-compiling keeps every build-time tool on the host
# where those prebuilts exist, and only the linked libraries come from the target.
#
# Target libraries come from multiarch rather than a separate sysroot, so the find rules
# stay permissive and CMAKE_LIBRARY_ARCHITECTURE is what points the search at
# /usr/lib/aarch64-linux-gnu. See docs/CI.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# clang finds the cross binutils and the target crt through the gcc cross toolchain that
# crossbuild-essential-arm64 installs
set(CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN /usr)
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN /usr)

# host tools (ispc, slangc, ShaderMake, dxc) must keep resolving to host binaries, while
# libraries and headers must resolve to the target's
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
