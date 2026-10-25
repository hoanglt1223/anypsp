# Vulkan GE backend (vk_gpu.cpp) inside anypsp_gpu: `--renderer vulkan` in the GPU runners.
# The GE shaders in runtime/host/shaders/ (shared with the GL backend) are compiled to SPIR-V
# with glslangValidator at build time and embedded. Included from gpu.cmake.

option(ANYPSP_BUILD_VULKAN "Build the Vulkan GE backend (needs the Vulkan headers, loader and glslangValidator)" ON)
if(NOT ANYPSP_BUILD_VULKAN)
    return()
endif()
find_package(Vulkan QUIET COMPONENTS glslangValidator)
if(NOT Vulkan_FOUND OR NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(STATUS "AnyPSP: Vulkan SDK or glslangValidator not found; building without the Vulkan GE backend")
    return()
endif()

set(ANYPSP_VK_DIR ${CMAKE_CURRENT_LIST_DIR})
set(ANYPSP_SHADER_DIR ${ANYPSP_VK_DIR}/../../host/shaders)
set(ANYPSP_VK_OUT ${CMAKE_CURRENT_BINARY_DIR}/vk-shaders)
file(MAKE_DIRECTORY ${ANYPSP_VK_OUT})
set(ANYPSP_VK_SOURCES ${ANYPSP_VK_DIR}/vk_gpu.cpp)
foreach(shader ge.vert ge.frag fullscreen.vert reinterpret.frag fill.frag alpha_to_stencil.frag copy.frag present.vert present.frag)
    string(REGEX MATCH "[^.]+$" stage "${shader}")
    string(REPLACE "." "_" symbol "${shader}")
    set(source ${ANYPSP_SHADER_DIR}/${shader}.inc)
    set(glsl ${ANYPSP_VK_OUT}/${shader}.glsl)
    set(spirv ${ANYPSP_VK_OUT}/${shader}.spv)
    set(embedded ${ANYPSP_VK_OUT}/${symbol}_spv.cpp)
    add_custom_command(OUTPUT ${glsl}
        COMMAND ${CMAKE_COMMAND} -DIN=${source} -DOUT=${glsl} -P ${PROJECT_SOURCE_DIR}/cmake/vk-shader-source.cmake
        DEPENDS ${source} ${PROJECT_SOURCE_DIR}/cmake/vk-shader-source.cmake
        VERBATIM)
    add_custom_command(OUTPUT ${spirv}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V -S ${stage} -o ${spirv} ${glsl}
        DEPENDS ${glsl}
        COMMENT "Compiling ${shader} to SPIR-V"
        VERBATIM)
    add_custom_command(OUTPUT ${embedded}
        COMMAND ${CMAKE_COMMAND} -DIN=${spirv} -DOUT=${embedded} -DNAME=anypsp_vk_${symbol}
            -P ${PROJECT_SOURCE_DIR}/cmake/embed-file.cmake
        DEPENDS ${spirv} ${PROJECT_SOURCE_DIR}/cmake/embed-file.cmake
        VERBATIM)
    list(APPEND ANYPSP_VK_SOURCES ${embedded})
endforeach()
target_sources(anypsp_gpu PRIVATE ${ANYPSP_VK_SOURCES})
target_include_directories(anypsp_gpu PUBLIC ${ANYPSP_VK_DIR})
target_compile_definitions(anypsp_gpu PUBLIC ANYPSP_VULKAN)
target_link_libraries(anypsp_gpu PUBLIC Vulkan::Vulkan)
