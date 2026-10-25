# Desktop GPU backend: the GE's GL renderer on OpenGL 3.3 core through SDL3,
# with an offscreen (hidden-window) runner that writes PNG frames and hashes.
# Included from the top-level CMakeLists.txt after the host library.

option(ANYPSP_BUILD_GPU "Build the desktop OpenGL GE backend and its runners (fetches SDL3)" ON)
if(NOT (ANYPSP_BUILD_HOST AND ANYPSP_BUILD_GPU))
    return()
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Same pinned release as the other anyport projects.
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(SDL3)

set(ANYPSP_GPU_DIR ${CMAKE_CURRENT_LIST_DIR})
add_library(anypsp_gpu STATIC
    ${ANYPSP_GPU_DIR}/../host/ge_gl.cpp
    ${ANYPSP_GPU_DIR}/gl_api.cpp
    ${ANYPSP_GPU_DIR}/gl_host.cpp
    ${ANYPSP_GPU_DIR}/in_between.cpp
    ${ANYPSP_GPU_DIR}/frame_output.cpp
    ${ANYPSP_GPU_DIR}/desktop.cpp
    ${ANYPSP_GPU_DIR}/disc.cpp
    ${ANYPSP_GPU_DIR}/input_map.cpp
    ${ANYPSP_GPU_DIR}/display.cpp
    ${ANYPSP_GPU_DIR}/display_loop.cpp
    ${ANYPSP_GPU_DIR}/patches.cpp
    ${ANYPSP_GPU_DIR}/cheats.cpp
    ${ANYPSP_GPU_DIR}/menu.cpp
    ${ANYPSP_GPU_DIR}/speed.cpp
    ${ANYPSP_GPU_DIR}/menu_host.cpp
    ${ANYPSP_GPU_DIR}/dialog_overlay.cpp)
target_include_directories(anypsp_gpu PUBLIC ${ANYPSP_GPU_DIR})
# The menu's font and translations, compiled in.
foreach(asset "regular_font;fonts/NotoSans-Regular.ttf" "bold_font;fonts/NotoSans-SemiBold.ttf" "strings;strings.tsv")
    list(GET asset 0 name)
    list(GET asset 1 file)
    set(source "${ANYPSP_GPU_DIR}/ui/${file}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/anypsp_ui_${name}.cpp")
    add_custom_command(OUTPUT "${generated}"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${source}" "-DOUT=${generated}" "-DNAME=anypsp_ui_${name}"
            -P "${PROJECT_SOURCE_DIR}/cmake/embed-file.cmake"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/embed-file.cmake"
        COMMENT "Embedding ui/${file}"
        VERBATIM)
    target_sources(anypsp_gpu PRIVATE "${generated}")
endforeach()
target_compile_definitions(anypsp_gpu PUBLIC ANYPSP_GL_DESKTOP)
target_link_libraries(anypsp_gpu PUBLIC anypsp_host SDL3::SDL3-static)
include(${ANYPSP_GPU_DIR}/vulkan/vulkan.cmake)
if(MSVC)
    target_compile_options(anypsp_gpu PRIVATE /EHsc /permissive- /W3)
    target_compile_definitions(anypsp_gpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Interpreter-only runner with a renderer (pspautotests GE tests, frame dumps).
add_executable(anypsp-interp-gpu ${ANYPSP_GPU_DIR}/main_gpu.cpp ${ANYPSP_GPU_DIR}/../host/no_generated.cpp
    ${ANYPSP_GPU_DIR}/../host/no_modules.cpp ${ANYPSP_GPU_DIR}/../host/no_dynamic_code.cpp)
target_link_libraries(anypsp-interp-gpu PRIVATE anypsp_gpu anypsp_loader)
if(MSVC)
    target_compile_options(anypsp-interp-gpu PRIVATE /EHsc /permissive- /W3)
    target_link_options(anypsp-interp-gpu PRIVATE /STACK:67108864)
endif()

# Playable launcher: the interpreter runner with a window, sound and input by default.
add_executable(anypsp-play ${ANYPSP_GPU_DIR}/main_gpu.cpp ${ANYPSP_GPU_DIR}/../host/no_generated.cpp
    ${ANYPSP_GPU_DIR}/../host/no_modules.cpp ${ANYPSP_GPU_DIR}/../host/no_dynamic_code.cpp)
target_compile_definitions(anypsp-play PRIVATE ANYPSP_PLAY)
target_link_libraries(anypsp-play PRIVATE anypsp_gpu anypsp_loader)
if(MSVC)
    target_compile_options(anypsp-play PRIVATE /EHsc /permissive- /W3)
    target_link_options(anypsp-play PRIVATE /STACK:67108864)
endif()

add_executable(anypsp_desktop_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/desktop_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/display_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/patches_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/cheats_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/menu_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/speed_tests.cpp)
target_include_directories(anypsp_desktop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
target_link_libraries(anypsp_desktop_tests PRIVATE anypsp_gpu)
target_compile_definitions(anypsp_desktop_tests PRIVATE ANYPSP_MENU_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/menu")
if(MSVC)
    target_compile_options(anypsp_desktop_tests PRIVATE /EHsc /permissive- /W3)
    target_compile_definitions(anypsp_desktop_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME anypsp_desktop_tests COMMAND anypsp_desktop_tests)

# Per-game runner with a renderer, next to anypsp-run-<name>.
if(TARGET anypsp_generated_${ANYPSP_RUNNER_NAME})
    set(ANYPSP_GPU_RUNNER anypsp-run-${ANYPSP_RUNNER_NAME}-gpu)
    add_executable(${ANYPSP_GPU_RUNNER} ${ANYPSP_GPU_DIR}/main_gpu.cpp)
    if(NOT EXISTS "${ANYPSP_RUNNER_GENERATED}/modules_table.cpp")
        target_sources(${ANYPSP_GPU_RUNNER} PRIVATE ${ANYPSP_GPU_DIR}/../host/no_modules.cpp)
    endif()
    if(NOT EXISTS "${ANYPSP_RUNNER_GENERATED}/dynamic_code.cpp")
        target_sources(${ANYPSP_GPU_RUNNER} PRIVATE ${ANYPSP_GPU_DIR}/../host/no_dynamic_code.cpp)
    endif()
    target_link_libraries(${ANYPSP_GPU_RUNNER} PRIVATE anypsp_generated_${ANYPSP_RUNNER_NAME} anypsp_gpu anypsp_loader)
    if(MSVC)
        target_compile_options(${ANYPSP_GPU_RUNNER} PRIVATE /EHsc /permissive- /W3)
        target_link_options(${ANYPSP_GPU_RUNNER} PRIVATE /STACK:67108864)
    endif()
endif()
