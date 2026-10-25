# libmain.so of an Android app made by `anypsp build --target android`: a runner's generated
# code, the HLE runtime, the GE's Vulkan backend (when the NDK's Vulkan and the host's
# glslangValidator are found) and OpenGL ES 3 backend, and the host in src/android_main.cpp.
# Included by cmake/hosts.cmake, in the top directory where the Vulkan shader rules live, when
# configuring with the NDK toolchain and ANYPSP_RUNNER_NAME.
add_library(main SHARED
    ${CMAKE_CURRENT_LIST_DIR}/src/android_main.cpp
    ${PROJECT_SOURCE_DIR}/runtime/host/ge_gl.cpp
    ${PROJECT_SOURCE_DIR}/runtime/gpu/disc.cpp
    ${PROJECT_SOURCE_DIR}/runtime/gpu/frame_output.cpp
    ${game_fallbacks})
target_include_directories(main PRIVATE ${PROJECT_SOURCE_DIR}/runtime/gpu ${PROJECT_SOURCE_DIR}/runtime/host)
target_compile_definitions(main PRIVATE ANYPSP_GAME_SHA256="${game_hash}")
target_link_libraries(main PRIVATE anypsp_generated_${ANYPSP_RUNNER_NAME} anypsp_host anypsp_loader SDL3::SDL3-static
    GLESv3 EGL android log)
# SDLActivity calls JNI_OnLoad and SDL_main in here; nothing in C++ refers to them.
target_link_options(main PRIVATE -Wl,--undefined=JNI_OnLoad -Wl,--no-undefined -Wl,-z,max-page-size=16384)
set_target_properties(main PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
anypsp_add_vulkan_host(main)
