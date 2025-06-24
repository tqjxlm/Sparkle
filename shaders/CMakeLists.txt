option(SHADER_DEBUG "Allow shader debug" OFF)

if(SHADER_DEBUG)
    message("Shader debug enabled")
    set(DCX_FLAGS ${DCX_FLAGS} -Zi -O0)
endif()

if (IOS)
    set(SPIRV_CROSS_FLAGS ${SPIRV_CROSS_FLAGS} --msl-ios)
endif()

if (ENABLE_METAL)
    set(SHADER_DEFINES ${SHADER_DEFINES} -DENABLE_METAL=1)
endif()

message("Vulkan SDK from environment " $ENV{VULKAN_SDK})

find_program(SPIRV_CROSS spirv-cross HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)
find_program(DXC dxc HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)

set(OUTPUT_SHADER_DIRECTORY ${PROJECT_RESOURCE_DIRECTORY}/shaders)
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/standard")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/screen")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/ray_trace")
file(MAKE_DIRECTORY "${OUTPUT_SHADER_DIRECTORY}/utilities")

file(GLOB_RECURSE HLSL_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/screen/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/standard/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/ray_trace/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/utilities/*.hlsl"
)

file(GLOB HLSL_INCLUDE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h.hlsl"
)

set(ALL_SPIRV_OUTPUTS)

# stage 1: hlsl->spirv
foreach(HLSL ${HLSL_SOURCE_FILES})
    cmake_path(GET HLSL FILENAME SHADER_NAME)
    cmake_path(RELATIVE_PATH HLSL BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE FILE_PATH_RELATIVE)
    
    set(SPIRV "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.spv")
    string(REPLACE "." ";" PARTS ${SHADER_NAME})
    list(GET PARTS 1 SHADER_TYPE)

    if (SHADER_TYPE STREQUAL "vs" OR SHADER_TYPE STREQUAL "ps" OR SHADER_TYPE STREQUAL "cs")
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${DXC} -spirv ${HLSL} -Fo ${SPIRV} -T ${SHADER_TYPE}_6_6 -E main ${DCX_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include
            COMMENT "Compiling ${SHADER_NAME} to SPIRV"
            DEPENDS ${HLSL} ${HLSL_INCLUDE_FILES}
        )
        list(APPEND ALL_SPIRV_OUTPUTS ${SPIRV})
    else()
        message(FATAL_ERROR "Invalid shader file name, please provide a valid shader type: " ${HLSL})
    endif()
endforeach(HLSL)

# stage 2 (metal only): spirv->metal
if (MACOS OR IOS)
    set(ALL_MTL_OUTPUTS)

    foreach(HLSL ${HLSL_SOURCE_FILES})
        cmake_path(GET HLSL FILENAME SHADER_NAME)
        cmake_path(RELATIVE_PATH HLSL BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE FILE_PATH_RELATIVE)

        set(SPIRV "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.spv")
        set(MSL "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.metal")

        cmake_path(GET HLSL PARENT_PATH PATH_NAME)
        cmake_path(GET PATH_NAME FILENAME SUB_PATH_NAME)
        if (SUB_PATH_NAME STREQUAL "ray_trace")
            # ray trace shaders need bindless
            set(BINDLESS_ARGS --msl-argument-buffer-tier 2 --msl-texture-buffer-native)
        else()
            set(BINDLESS_ARGS )
        endif()

        # not used for now due to spirv-cross limitation
        # set(ARGUMENT_BUFFER_ARGS "--msl-argument-buffers --msl-decoration-binding --msl-force-active-argument-buffer-resources")

        add_custom_command(
            OUTPUT ${MSL}
            COMMAND ${SPIRV_CROSS} --msl ${SPIRV} --output ${MSL} --msl-version 30000 --flip-vert-y ${BINDLESS_ARGS} ${SPIRV_CROSS_FLAGS}
            COMMENT "Compiling ${SHADER_NAME} to Metal"
            DEPENDS ${SPIRV}
        )
        list(APPEND ALL_MTL_OUTPUTS ${MSL})
    endforeach(HLSL)

    add_custom_target(
        compile-metal-shaders ALL
        DEPENDS ${ALL_MTL_OUTPUTS}
    )

    add_dependencies(cook-resources compile-metal-shaders)
else()
    add_custom_target(
        compile-shaders ALL
        DEPENDS ${ALL_SPIRV_OUTPUTS}
    )

    add_dependencies(cook-resources compile-shaders)
endif()
