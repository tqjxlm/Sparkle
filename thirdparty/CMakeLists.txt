include(FetchContent)

function(suppress_warnings TARGET_NAME)
    if(TARGET ${TARGET_NAME})
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${TARGET_NAME} PRIVATE -w)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${TARGET_NAME} PRIVATE /W0)
        endif()
    endif()
endfunction()

# push compile options
set(OLD_CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# we do not want to see warnings from third party libraries
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /Zc:preprocessor /W0")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
endif()

# ------------------ built-in dependencies ------------------

FILE(GLOB THIRD_PARTY_SOURCES
    imgui/*.cpp
    imgui/misc/cpp/imgui_stdlib.cpp
    spirv_reflect/spirv_reflect.cpp
    hash-library/src/*.cpp
    bc7enc_rdo/bc7enc.cpp
    bc7enc_rdo/bc7decomp.cpp
)

if (GLFW)
    FILE(GLOB IMGUI_SOURCES
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_vulkan.h
        imgui/backends/imgui_impl_glfw.cpp
        imgui/backends/imgui_impl_glfw.h
    )
elseif (MACOS)
    FILE(GLOB IMGUI_SOURCES
        imgui/backends/imgui_impl_metal.mm
        imgui/backends/imgui_impl_metal.h
        #macos feeds imgui through InputManager and needs no platform backend
    )
elseif (IOS)
    FILE(GLOB IMGUI_SOURCES
        imgui/backends/imgui_impl_metal.mm
        imgui/backends/imgui_impl_metal.h
        #ios platform support is located at platforms/source/iOS
    )
elseif (ANDROID)
    FILE(GLOB IMGUI_SOURCES
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_vulkan.h
        imgui/backends/imgui_impl_android.cpp
        imgui/backends/imgui_impl_android.h
    )
endif()

set(THIRD_PARTY_SOURCES ${THIRD_PARTY_SOURCES} ${IMGUI_SOURCES})
add_library(sparkle_thirdparty STATIC ${THIRD_PARTY_SOURCES})

# ------------------ tinyusdz ------------------

if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(TINYUSDZ_CXX_MP_FLAG OFF CACHE BOOL "Build with /MP flag enabled" FORCE)
    set(TINYUSDZ_NO_WERROR ON)
endif()

# the tinyexr v3 C implementation does not compile with clang-cl (__cpuid arity mismatch),
# and tinyusdz's image loader only uses the v1 API anyway
set(TINYUSDZ_USE_TINYEXR_V3 OFF CACHE BOOL "Use TinyEXR v3 API" FORCE)

add_subdirectory(tinyusdz tinyusdz)

target_include_directories(sparkle_thirdparty SYSTEM PUBLIC "tinyusdz/src")

target_link_libraries(sparkle_thirdparty PRIVATE tinyusdz::tinyusdz_static)

# ------------------ vulkan ------------------

if (ENABLE_VULKAN)
    target_compile_definitions(sparkle_thirdparty PUBLIC VULKAN_USE_VOLK=1 IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

    if (ANDROID)
        # android will use vulkan SDK provided by android toolchain
        target_link_libraries(sparkle_thirdparty PRIVATE
            # vulkan
            game-activity::game-activity_static
            android
            log
        )

    elseif(GLFW)
        target_link_libraries(sparkle_thirdparty PRIVATE Vulkan::Vulkan glfw)
    endif()
endif()

# ------------------ mimalloc ------------------

set(MI_BUILD_SHARED OFF)
set(MI_BUILD_OBJECT ON)
set(MI_BUILD_STATIC OFF)
set(MI_BUILD_TESTS OFF)
set(MI_OVERRIDE ON)
if (WIN32)
    set(MI_USE_CXX ON)
endif()
if (ANDROID)
    set(MI_LOCAL_DYNAMIC_TLS ON)
endif()
add_subdirectory(mimalloc)

suppress_warnings(mimalloc-obj)

target_link_libraries(sparkle_thirdparty PUBLIC mimalloc-obj)

# ------------------ debugger ------------------

# archive download instead of a git clone: much faster and one less failure point during configure
FetchContent_Declare(
    cpptrace
    DOWNLOAD_EXTRACT_TIMESTAMP 1
    DOWNLOAD_NO_PROGRESS 1
    URL https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v1.0.4.zip
)

# Check if compiler supports C++20 modules
# Note: clang-cl doesn't fully support CMake's module dependency scanning yet
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.28")
    # MSVC 16.8+ supports modules
    set(CPPTRACE_DISABLE_CXX_20_MODULES OFF CACHE BOOL "Disable C++20 modules in cpptrace" FORCE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0")
    # Regular Clang 16+ has reasonable module support (exclude clang-cl for now)
    set(CPPTRACE_DISABLE_CXX_20_MODULES OFF CACHE BOOL "Disable C++20 modules in cpptrace" FORCE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
    # GCC 11+ has experimental module support
    set(CPPTRACE_DISABLE_CXX_20_MODULES OFF CACHE BOOL "Disable C++20 modules in cpptrace" FORCE)
else()
    # Disable C++20 modules for compatibility with current compiler/CMake setup
    set(CPPTRACE_DISABLE_CXX_20_MODULES ON CACHE BOOL "Disable C++20 modules in cpptrace" FORCE)
endif()

FetchContent_MakeAvailable(cpptrace)

suppress_warnings(cpptrace-lib)
suppress_warnings(dwarf)

target_link_libraries(sparkle_thirdparty PUBLIC $<$<CONFIG:Debug>:cpptrace::cpptrace>)

# ------------------ profiler ------------------

if (ENABLE_PROFILER)
    set(TRACY_ENABLE ON)
    set(TRACY_ON_DEMAND ON)
    add_subdirectory(tracy)

    suppress_warnings(TracyClient)

    target_link_libraries(sparkle_thirdparty PUBLIC Tracy::TracyClient)

    target_compile_definitions(sparkle PRIVATE ENABLE_PROFILER=1)
else()
    target_compile_definitions(sparkle PRIVATE ENABLE_PROFILER=0)
endif()

# ------------------ astcenc (ASTC texture encoder/decoder) ------------------

# one explicit SIMD backend per target arch. never ISA_NATIVE: released binaries must not
# carry -march=native. UNIVERSAL_BUILD (Darwin default) builds every ISA; force single-arch.
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(SPARKLE_ASTCENC_TARGET astcenc-neon-static)
else()
    set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
    set(SPARKLE_ASTCENC_TARGET astcenc-sse4.1-static)
endif()
add_subdirectory(astc-encoder)
suppress_warnings(${SPARKLE_ASTCENC_TARGET})
target_link_libraries(sparkle_thirdparty PUBLIC ${SPARKLE_ASTCENC_TARGET})

# ------------------ NRD (NVIDIA Real-time Denoiser) ------------------

# Real-time GI denoiser. Native-API path: its SPIR-V shaders are cross-compiled to Metal by our shader
# pipeline; NRI (D3D/VK-only, unusable on Metal) stays off. Static lib so there is no dylib to bundle.
# DXC (HLSL->SPIR-V) comes from the Vulkan SDK already in our build deps. See docs/Nrd.md.
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_NRI OFF CACHE BOOL "" FORCE)
# R10G10B10A2_UNORM (NRD's default): halves the bandwidth of the most-sampled ReBLUR texture vs
# RGBA16_SNORM; the oct encode/decode is replicated in shaders/include/nrd.h.slang. Set "4"
# (RGBA16_SNORM, raw float4 writes, no packing) to rule the packing out when debugging
# normal-related artifacts.
set(NRD_NORMAL_ENCODING "2" CACHE STRING "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
if(DEFINED ENV{VULKAN_SDK})
    if(WIN32)
        set(SHADERMAKE_DXC_VK_PATH "$ENV{VULKAN_SDK}/Bin/dxc.exe" CACHE FILEPATH "" FORCE)
    else()
        set(SHADERMAKE_DXC_VK_PATH "$ENV{VULKAN_SDK}/bin/dxc" CACHE FILEPATH "" FORCE)
    endif()
endif()

if(IOS)
    # the ShaderMake executable runs on the build host during the shader cook. it must not be
    # built by the iOS project in any form: an iOS binary cannot run on the host, and the Xcode
    # generator demands code signing for executable targets (both in-project and via
    # ExternalProject). provide everything NRD expects up front so ShaderMake's own CMake never
    # runs: the ShaderMake target (cook dependency), the ShaderMakeBlob library, and
    # SHADERMAKE_PATH pointing at a host binary built at configure time.
    include(FetchContent)
    FetchContent_Populate(
        shadermake
        DOWNLOAD_EXTRACT_TIMESTAMP 1
        DOWNLOAD_NO_PROGRESS 1
        URL https://github.com/NVIDIA-RTX/ShaderMake/archive/18f5a344e7ca8fa65daaf079d07bc8ce38453e05.zip
    )

    set(SHADERMAKE_HOST_BUILD_DIR ${CMAKE_BINARY_DIR}/shadermake_host)
    set(SHADERMAKE_HOST_BINARY ${SHADERMAKE_HOST_BUILD_DIR}/ShaderMake)
    if(NOT EXISTS ${SHADERMAKE_HOST_BINARY})
        message(STATUS "Building ShaderMake for the build host")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -S ${shadermake_SOURCE_DIR}/ShaderMake -B ${SHADERMAKE_HOST_BUILD_DIR}
                    -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
            RESULT_VARIABLE SHADERMAKE_HOST_CONFIGURE_RESULT)
        if(SHADERMAKE_HOST_CONFIGURE_RESULT)
            message(FATAL_ERROR "Failed to configure host ShaderMake")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${SHADERMAKE_HOST_BUILD_DIR} -j
            RESULT_VARIABLE SHADERMAKE_HOST_BUILD_RESULT)
        if(SHADERMAKE_HOST_BUILD_RESULT OR NOT EXISTS ${SHADERMAKE_HOST_BINARY})
            message(FATAL_ERROR "Failed to build host ShaderMake")
        endif()
    endif()

    add_library(ShaderMakeBlob STATIC ${shadermake_SOURCE_DIR}/ShaderMake/ShaderBlob.cpp)
    target_include_directories(ShaderMakeBlob SYSTEM PUBLIC ${shadermake_SOURCE_DIR})
    set_target_properties(ShaderMakeBlob PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_custom_target(ShaderMake)

    set(SHADERMAKE_PATH ${SHADERMAKE_HOST_BINARY} CACHE INTERNAL "" FORCE)
endif()

add_subdirectory(NRD)
suppress_warnings(NRD)
target_link_libraries(sparkle_thirdparty PUBLIC NRD)
target_include_directories(sparkle_thirdparty SYSTEM PUBLIC NRD/Include)

# ------------------ finalize ------------------

target_include_directories(sparkle_thirdparty SYSTEM PUBLIC
    stb
    argparse/include
    bvh/src
    thread-pool/include
    spdlog/include
    vma/include
    tinygltf
    eigen
    volk
    # TODO: it crashes we have multiple versions of fast_float. for now we just use the one in tinyusdz.
    tinyusdz/src/external/fast_float/include
    json/single_include
    imgui
    imgui/backends
    imgui/misc/cpp
    spirv_reflect
    magic_enum/include
    xoshiro_cpp
    hash-library/include
    IconFontCppHeaders
    astc-encoder/Source
    bc7enc_rdo
)

target_link_libraries(sparkle PUBLIC sparkle_thirdparty)

# some IDEs require this to show third party files
set(THIRD_PARTY_SOURCES ${THIRD_PARTY_SOURCES} PARENT_SCOPE)

# pop the original compiler flags
set(CMAKE_CXX_FLAGS "${OLD_CMAKE_CXX_FLAGS}")
