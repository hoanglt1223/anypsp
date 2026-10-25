# Hosts and packaging, included at the end of the top-level CMakeLists.txt:
#  - the `anypsp` front end (tools/anypsp), which `anypsp build` drives this project with;
#  - anypsp-game: the program `anypsp build` packages, a runner's generated code with the desktop
#    host and a launcher that finds the game files next to it (tools/anypsp/game_main.cpp);
#  - on Android, libmain.so for the APK (platform/android);
#  - the install layout release packages use (cmake/install.cmake).

option(ANYPSP_BUILD_DRIVER "Build the anypsp front end (anypsp build) and its tests" ON)
set(ANYPSP_GAME_SHA256 "" CACHE STRING "SHA-256 of the executable the runner's code was translated from (anypsp build)")

if(ANYPSP_BUILD_DRIVER AND NOT ANDROID)
    add_subdirectory(tools/anypsp)
endif()

# The Vulkan GE backend (runtime/gpu/vulkan/vulkan.cmake) and the swapchain presenter in a GLES host.
function(anypsp_add_vulkan_host target)
    if(DEFINED ANYPSP_VK_SOURCES AND TARGET Vulkan::Vulkan)
        target_sources(${target} PRIVATE ${ANYPSP_VK_SOURCES} ${PROJECT_SOURCE_DIR}/platform/android/src/vk_present.cpp)
        target_include_directories(${target} PRIVATE ${ANYPSP_VK_DIR})
        target_compile_definitions(${target} PRIVATE ANYPSP_VULKAN)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endif()
endfunction()

if(ANYPSP_BUILD_HOST AND NOT ANYPSP_RUNNER_NAME STREQUAL "" AND TARGET anypsp_generated_${ANYPSP_RUNNER_NAME})
    if(ANYPSP_GAME_SHA256 STREQUAL "")
        set(game_hash "unknown")
    else()
        set(game_hash "${ANYPSP_GAME_SHA256}")
    endif()
    set(game_fallbacks)
    if(NOT EXISTS "${ANYPSP_RUNNER_GENERATED}/modules_table.cpp")
        list(APPEND game_fallbacks ${PROJECT_SOURCE_DIR}/runtime/host/no_modules.cpp)
    endif()
    if(NOT EXISTS "${ANYPSP_RUNNER_GENERATED}/dynamic_code.cpp")
        list(APPEND game_fallbacks ${PROJECT_SOURCE_DIR}/runtime/host/no_dynamic_code.cpp)
    endif()

    if(ANDROID)
        include(${PROJECT_SOURCE_DIR}/platform/android/android.cmake)
    elseif(TARGET anypsp_gpu)
        if(UNIX AND NOT APPLE)
            # The Android host on Mesa's OpenGL ES 3: GLES-only ARM handhelds, and the Android path on a desktop.
            find_library(ANYPSP_GLESV2 GLESv2)
            if(ANYPSP_GLESV2)
                add_executable(anypsp-game-gles ${PROJECT_SOURCE_DIR}/platform/android/src/android_main.cpp
                    ${PROJECT_SOURCE_DIR}/runtime/host/ge_gl.cpp ${PROJECT_SOURCE_DIR}/runtime/gpu/disc.cpp
                    ${PROJECT_SOURCE_DIR}/runtime/gpu/frame_output.cpp ${game_fallbacks})
                target_include_directories(anypsp-game-gles PRIVATE ${PROJECT_SOURCE_DIR}/runtime/gpu ${PROJECT_SOURCE_DIR}/runtime/host)
                target_compile_definitions(anypsp-game-gles PRIVATE ANYPSP_GAME_SHA256="${game_hash}")
                target_link_libraries(anypsp-game-gles PRIVATE anypsp_generated_${ANYPSP_RUNNER_NAME} anypsp_host anypsp_loader
                    SDL3::SDL3-static ${ANYPSP_GLESV2})
                anypsp_add_vulkan_host(anypsp-game-gles)
            endif()
        endif()
        # The launcher is its own object library: main_gpu.cpp's main() is renamed for anypsp-game only.
        add_library(anypsp_game_launcher OBJECT ${PROJECT_SOURCE_DIR}/tools/anypsp/game_main.cpp)
        target_compile_definitions(anypsp_game_launcher PRIVATE ANYPSP_GAME_SHA256="${game_hash}")
        target_link_libraries(anypsp_game_launcher PRIVATE anypsp_gpu)
        target_include_directories(anypsp_game_launcher PRIVATE ${PROJECT_SOURCE_DIR}/runtime/gpu)

        add_executable(anypsp-game ${PROJECT_SOURCE_DIR}/runtime/gpu/main_gpu.cpp ${game_fallbacks}
            $<TARGET_OBJECTS:anypsp_game_launcher>)
        target_compile_definitions(anypsp-game PRIVATE ANYPSP_PLAY main=anypsp_runner_main)
        target_link_libraries(anypsp-game PRIVATE anypsp_generated_${ANYPSP_RUNNER_NAME} anypsp_gpu anypsp_loader)
        if(MSVC)
            target_compile_options(anypsp-game PRIVATE /EHsc /permissive- /W3)
            # A window program without a console that still starts at main().
            set_target_properties(anypsp-game PROPERTIES WIN32_EXECUTABLE ON)
            target_link_options(anypsp-game PRIVATE /STACK:67108864 /ENTRY:mainCRTStartup)
        elseif(WIN32)
            target_link_options(anypsp-game PRIVATE -mwindows)
        endif()
    endif()
endif()

# Long scripted UNH 1 run (tests/unh1-regression.py): skipped without the user's own disc. Set
# ANYPSP_UNH1_PROGRAM to a program from `anypsp build` to also check its frames against the interpreter.
if(Python3_Interpreter_FOUND AND TARGET anypsp-play AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME unh1_regression
             COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/unh1-regression.py --play $<TARGET_FILE:anypsp-play>
                     --work ${CMAKE_CURRENT_BINARY_DIR}/unh1-regression)
    set_tests_properties(unh1_regression PROPERTIES LABELS slow SKIP_RETURN_CODE 77 TIMEOUT 7200)
endif()

if(NOT ANDROID)
    include(${CMAKE_CURRENT_LIST_DIR}/install.cmake)
endif()
