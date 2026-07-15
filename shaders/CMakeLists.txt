option(SHADER_DEBUG "Allow shader debug" OFF)

if(SHADER_DEBUG)
    message("Shader debug enabled")
    set(SLANGC_FLAGS ${SLANGC_FLAGS} -g -O0)
endif()

if (ENABLE_METAL)
    set(SHADER_DEFINES ${SHADER_DEFINES} -DENABLE_METAL=1)
endif()

message("Vulkan SDK from environment " $ENV{VULKAN_SDK})

find_program(SPIRV_CROSS spirv-cross HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)
if(DEFINED ENV{SLANGC})
    set(SLANGC $ENV{SLANGC})
else()
    find_program(SLANGC slangc HINTS /usr/bin /usr/local/bin)
endif()

set(OUTPUT_SHADER_DIRECTORY ${PROJECT_RESOURCE_DIRECTORY}/shaders)
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/standard")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/screen")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/ray_trace")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/utilities")

file(GLOB_RECURSE SLANG_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.ps.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vs.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cs.slang"
)

file(GLOB_RECURSE SLANG_INCLUDE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h.slang"
)

set(ALL_SPIRV_OUTPUTS)

# stage 1: slang->spirv
foreach(SLANG ${SLANG_SOURCE_FILES})
    cmake_path(GET SLANG FILENAME SHADER_NAME)
    cmake_path(RELATIVE_PATH SLANG BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE FILE_PATH_RELATIVE)
    
    set(SPIRV "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.spv")
    string(REPLACE "." ";" PARTS ${SHADER_NAME})
    list(GET PARTS 1 SHADER_TYPE)

    if (SHADER_TYPE STREQUAL "vs")
        set(STAGE "vertex")
    elseif (SHADER_TYPE STREQUAL "ps")
        set(STAGE "fragment")
    elseif (SHADER_TYPE STREQUAL "cs")
        set(STAGE "compute")
    else()
        message(FATAL_ERROR "Invalid shader file name, please provide a valid shader type (vs, ps, cs): " ${SLANG})
    endif()

    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${SLANGC} ${SLANG} -target spirv -o ${SPIRV} -entry shader_main ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include -profile spirv_1_3
        COMMENT "Compiling ${SHADER_NAME} to SPIRV: ${SLANGC} ${SLANG} -target spirv -o ${SPIRV} -entry shader_main ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include -profile spirv_1_3"
        DEPENDS ${SLANG} ${SLANG_INCLUDE_FILES}
    )
    list(APPEND ALL_SPIRV_OUTPUTS ${SPIRV})
endforeach(SLANG)

# stage 2 (metal only): slang->metal directly.
# ray_trace shaders still go slang->spirv->spirv-cross->metal: slang cannot emit valid MSL
# for multiple unsized bindless arrays yet. its MSL also does not bake a vertex y-flip,
# so the metal RHI flips the viewport instead (see MetalRenderPass).
# the branch follows the graphics backend, not the host OS: a glfw build on a macOS host
# targets Vulkan and needs the SPIR-V outputs below.
if (ENABLE_METAL)
    set(ALL_MTL_OUTPUTS)

    if (IOS)
        set(SPIRV_CROSS_FLAGS ${SPIRV_CROSS_FLAGS} --msl-ios)
    endif()

    foreach(SLANG ${SLANG_SOURCE_FILES})
        cmake_path(GET SLANG FILENAME SHADER_NAME)
        cmake_path(RELATIVE_PATH SLANG BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE FILE_PATH_RELATIVE)

        set(SPIRV "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.spv")
        set(MSL "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.metal")

        string(REPLACE "." ";" PARTS ${SHADER_NAME})
        list(GET PARTS 1 SHADER_TYPE)
        if (SHADER_TYPE STREQUAL "vs")
            set(STAGE "vertex")
        elseif (SHADER_TYPE STREQUAL "ps")
            set(STAGE "fragment")
        else()
            set(STAGE "compute")
        endif()

        cmake_path(GET SLANG PARENT_PATH PATH_NAME)
        cmake_path(GET PATH_NAME FILENAME SUB_PATH_NAME)
        if (SUB_PATH_NAME STREQUAL "ray_trace")
            # ray trace shaders need bindless
            set(SPIRV_CROSS_FLAGS ${SPIRV_CROSS_FLAGS} --msl-argument-buffer-tier 2 --msl-texture-buffer-native)

            add_custom_command(
                OUTPUT ${MSL}
                COMMAND ${SPIRV_CROSS} --msl ${SPIRV} --output ${MSL} --msl-version 30000 --flip-vert-y ${SPIRV_CROSS_FLAGS}
                COMMENT "Compiling ${SHADER_NAME} to Metal: ${SPIRV_CROSS} --msl ${SPIRV} --output ${MSL} --msl-version 30000 --flip-vert-y ${SPIRV_CROSS_FLAGS}"
                DEPENDS ${SPIRV}
            )
        else()
            add_custom_command(
                OUTPUT ${MSL}
                COMMAND ${SLANGC} ${SLANG} -target metal -o ${MSL} -entry shader_main -stage ${STAGE} ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include
                COMMENT "Compiling ${SHADER_NAME} to Metal: ${SLANGC} ${SLANG} -target metal -o ${MSL} -entry shader_main -stage ${STAGE} ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include"
                DEPENDS ${SLANG} ${SLANG_INCLUDE_FILES}
            )
        endif()
        list(APPEND ALL_MTL_OUTPUTS ${MSL})
    endforeach(SLANG)

    add_custom_target(
        compile-metal-shaders ALL
        DEPENDS ${ALL_MTL_OUTPUTS}
    )

    add_dependencies(cook-resources compile-metal-shaders)

    # NRD's shaders are cooked SPIR-V->MSL at build time into the intermediate shader directory (see
    # docs/Nrd.md). The cook tool runs on the build HOST even when targeting iOS; its ordering on
    # NRDShaders is declared at root scope because thirdparty is added after this subdirectory.
    if (IOS)
        set(NRD_COOKED_PLATFORM ios)
    else()
        set(NRD_COOKED_PLATFORM macos)
    endif()
    set(NRD_COOK_TOOL ${CMAKE_CURRENT_BINARY_DIR}/nrd_msl_cook)
    add_custom_command(
        OUTPUT ${NRD_COOK_TOOL}
        # explicit sdk and target: xcode script phases export SDKROOT and IPHONEOS_DEPLOYMENT_TARGET
        # (when targeting ios), either of which silently retargets this host tool to ios
        COMMAND xcrun -sdk macosx clang++ -target ${CMAKE_HOST_SYSTEM_PROCESSOR}-apple-macosx
            -std=c++17 -O1 ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/nrd_msl_cook.cpp
            -I $ENV{VULKAN_SDK}/include
            $ENV{VULKAN_SDK}/lib/libspirv-cross-msl.a
            $ENV{VULKAN_SDK}/lib/libspirv-cross-glsl.a
            $ENV{VULKAN_SDK}/lib/libspirv-cross-core.a
            -o ${NRD_COOK_TOOL}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/nrd_msl_cook.cpp
        COMMENT "Building nrd_msl_cook (host tool)"
    )
    file(GLOB NRD_SPIRV_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/thirdparty/NRD/_Shaders/*.cs.spirv.h")
    set(NRD_COOK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/nrd_cook_${NRD_COOKED_PLATFORM}.stamp)
    add_custom_command(
        OUTPUT ${NRD_COOK_STAMP}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/cook_nrd_shaders.py
            --shaders-dir ${CMAKE_SOURCE_DIR}/thirdparty/NRD/_Shaders
            --tool ${NRD_COOK_TOOL}
            --platform ${NRD_COOKED_PLATFORM}
            --nrd-header ${CMAKE_SOURCE_DIR}/thirdparty/NRD/Include/NRD.h
            --output ${OUTPUT_SHADER_DIRECTORY}/nrd/cooked
            --blob-prefix REBLUR_ --blob-prefix Clear
        COMMAND ${CMAKE_COMMAND} -E touch ${NRD_COOK_STAMP}
        DEPENDS ${NRD_COOK_TOOL} ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/cook_nrd_shaders.py ${NRD_SPIRV_HEADERS}
        COMMENT "Cooking NRD shaders (${NRD_COOKED_PLATFORM})"
    )
    add_custom_target(cook-nrd-shaders DEPENDS ${NRD_COOK_STAMP})
    add_dependencies(cook-resources cook-nrd-shaders)
else()
    add_custom_target(
        compile-shaders ALL
        DEPENDS ${ALL_SPIRV_OUTPUTS}
    )

    add_dependencies(cook-resources compile-shaders)

    if (ANDROID)
        # NRD's SPIR-V is packed as-is for the Vulkan backend; no host cross-compile tool needed
        file(GLOB NRD_SPIRV_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/thirdparty/NRD/_Shaders/*.cs.spirv.h")
        set(NRD_COOK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/nrd_cook_android.stamp)
        add_custom_command(
            OUTPUT ${NRD_COOK_STAMP}
            COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/cook_nrd_shaders.py
                --shaders-dir ${CMAKE_SOURCE_DIR}/thirdparty/NRD/_Shaders
                --platform android
                --nrd-header ${CMAKE_SOURCE_DIR}/thirdparty/NRD/Include/NRD.h
                --output ${OUTPUT_SHADER_DIRECTORY}/nrd/cooked
                --blob-prefix REBLUR_ --blob-prefix Clear
            COMMAND ${CMAKE_COMMAND} -E touch ${NRD_COOK_STAMP}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/nrd/cook/cook_nrd_shaders.py ${NRD_SPIRV_HEADERS}
            COMMENT "Cooking NRD shaders (android)"
        )
        add_custom_target(cook-nrd-shaders DEPENDS ${NRD_COOK_STAMP})
        add_dependencies(cook-resources cook-nrd-shaders)
    endif()
endif()
