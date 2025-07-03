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
find_program(SLANGC slangc HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/bin/)

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
        COMMAND ${SLANGC} ${SLANG} -target spirv -o ${SPIRV} -entry main ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include -profile spirv_1_4
        COMMENT "Compiling ${SHADER_NAME} to SPIRV: ${SLANGC} ${SLANG} -target spirv -o ${SPIRV} -entry main ${SLANGC_FLAGS} ${SHADER_DEFINES} -I ${CMAKE_CURRENT_SOURCE_DIR}/include -profile spirv_1_4"
        DEPENDS ${SLANG} ${SLANG_INCLUDE_FILES}
    )
    list(APPEND ALL_SPIRV_OUTPUTS ${SPIRV})
endforeach(SLANG)

# TODO: use slang to compile metal directly when it is ready
# stage 2 (metal only): spirv->metal
if (MACOS OR IOS)
    set(ALL_MTL_OUTPUTS)

    if (IOS)
        set(SPIRV_CROSS_FLAGS ${SPIRV_CROSS_FLAGS} --msl-ios)
    endif()

    foreach(SLANG ${SLANG_SOURCE_FILES})
        cmake_path(GET SLANG FILENAME SHADER_NAME)
        cmake_path(RELATIVE_PATH SLANG BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE FILE_PATH_RELATIVE)

        set(SPIRV "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.spv")
        set(MSL "${OUTPUT_SHADER_DIRECTORY}/${FILE_PATH_RELATIVE}.metal")

        cmake_path(GET SLANG PARENT_PATH PATH_NAME)
        cmake_path(GET PATH_NAME FILENAME SUB_PATH_NAME)
        if (SUB_PATH_NAME STREQUAL "ray_trace")
            # ray trace shaders need bindless
            set(SPIRV_CROSS_FLAGS ${SPIRV_CROSS_FLAGS} --msl-argument-buffer-tier 2 --msl-texture-buffer-native)
        endif()

        # not used for now due to spirv-cross limitation
        # set(ARGUMENT_BUFFER_ARGS "--msl-argument-buffers --msl-decoration-binding --msl-force-active-argument-buffer-resources")

        add_custom_command(
            OUTPUT ${MSL}
            COMMAND ${SPIRV_CROSS} --msl ${SPIRV} --output ${MSL} --msl-version 30000 --flip-vert-y ${SPIRV_CROSS_FLAGS}
            COMMENT "Compiling ${SHADER_NAME} to Metal: ${SPIRV_CROSS} --msl ${SPIRV} --output ${MSL} --msl-version 30000 --flip-vert-y ${SPIRV_CROSS_FLAGS}"
            DEPENDS ${SPIRV}
        )
        list(APPEND ALL_MTL_OUTPUTS ${MSL})
    endforeach(SLANG)

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
