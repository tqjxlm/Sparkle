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
        imgui/backends/imgui_impl_osx.mm
        imgui/backends/imgui_impl_osx.h
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

FetchContent_Declare(
    cpptrace
    GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
    GIT_TAG        v1.0.4
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
)

target_link_libraries(sparkle PUBLIC sparkle_thirdparty)

# some IDEs require this to show third party files
set(THIRD_PARTY_SOURCES ${THIRD_PARTY_SOURCES} PARENT_SCOPE)

# pop the original compiler flags
set(CMAKE_CXX_FLAGS "${OLD_CMAKE_CXX_FLAGS}")
