cmake_minimum_required(VERSION 3.20)
project(AnyPSP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_subdirectory(recomp)

function(anypsp_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /wd4324)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(anypsp_loader STATIC
    recomp/loader/elf_prx.cpp
    recomp/loader/nid_db.cpp
    recomp/loader/relocs.cpp
)
target_include_directories(anypsp_loader PUBLIC recomp/loader)
target_link_libraries(anypsp_loader PUBLIC psprecomp_core)
target_compile_definitions(anypsp_loader PRIVATE ANYPSP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
anypsp_warnings(anypsp_loader)

add_executable(anypsp_tests
    tests/unit/main.cpp
    tests/unit/loader_tests.cpp
    tests/unit/analysis_tests.cpp
    tests/unit/feedback_tests.cpp
    tests/unit/vram_tracking_tests.cpp
)
target_include_directories(anypsp_tests PRIVATE tests/unit)
target_link_libraries(anypsp_tests PRIVATE anypsp_analysis)
anypsp_warnings(anypsp_tests)
add_test(NAME anypsp_tests COMMAND anypsp_tests)

add_library(anypsp_analysis STATIC
    recomp/analysis/cfg.cpp
    recomp/analysis/functions.cpp
    recomp/analysis/jumptables.cpp
)
target_include_directories(anypsp_analysis PUBLIC recomp/analysis)
target_link_libraries(anypsp_analysis PUBLIC anypsp_loader)
anypsp_warnings(anypsp_analysis)

add_executable(anypsp-analyze recomp/tools/anypsp_analyze.cpp)
target_link_libraries(anypsp-analyze PRIVATE anypsp_analysis)
anypsp_warnings(anypsp-analyze)

# psp_recomp --auto loads modules through the AnyPSP loader.
target_link_libraries(psp_recomp PRIVATE anypsp_loader)

# Renderer-free native host: the HLE kernel and the software GE, no window, no audio.
option(ANYPSP_BUILD_HOST "Build the headless native host library" ON)
if(ANYPSP_BUILD_HOST)
    set(ANYPSP_AT3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/at3_standalone)
    add_library(anypsp_at3 STATIC
        ${ANYPSP_AT3_DIR}/atrac.cpp ${ANYPSP_AT3_DIR}/atrac3.cpp ${ANYPSP_AT3_DIR}/atrac3plus.cpp
        ${ANYPSP_AT3_DIR}/atrac3plusdec.cpp ${ANYPSP_AT3_DIR}/atrac3plusdsp.cpp ${ANYPSP_AT3_DIR}/get_bits.cpp
        ${ANYPSP_AT3_DIR}/compat.cpp ${ANYPSP_AT3_DIR}/fft.cpp ${ANYPSP_AT3_DIR}/mem.cpp)
    target_include_directories(anypsp_at3 PRIVATE runtime/third_party/at3_shim)
    target_include_directories(anypsp_at3 PUBLIC ${ANYPSP_AT3_DIR})
    if(MSVC)
        target_compile_options(anypsp_at3 PRIVATE /w)
    else()
        target_compile_options(anypsp_at3 PRIVATE -w)
    endif()

    add_library(anypsp_host STATIC
        runtime/host/atrac.cpp runtime/host/devices.cpp runtime/host/font.cpp runtime/host/ge.cpp
        runtime/host/ge_interp.cpp runtime/host/ge_surfaces.cpp runtime/host/ge_worker.cpp runtime/host/io.cpp runtime/host/kernel.cpp runtime/host/kernel_callbacks.cpp runtime/host/kernel_lwmutex.cpp runtime/host/kernel_msgpipe.cpp runtime/host/kernel_objects.cpp runtime/host/kernel_sync.cpp runtime/host/kernel_threads.cpp runtime/host/kernel_tls.cpp runtime/host/kernel_vtimer.cpp runtime/host/modules.cpp
        runtime/host/mp3.cpp runtime/host/mpeg.cpp runtime/host/net.cpp runtime/host/osk.cpp runtime/host/power.cpp runtime/host/rtc.cpp runtime/host/sas.cpp runtime/host/savedata.cpp runtime/host/webfs.cpp)
    target_include_directories(anypsp_host PUBLIC runtime/host)
    # minimp3 (CC0), header-only, for sceMp3.
    target_include_directories(anypsp_host PRIVATE runtime/third_party/minimp3)
    # Bundled replacements for the PSP's system fonts (runtime/host/fonts, see font.cpp), compiled in.
    file(GLOB ANYPSP_FALLBACK_FONTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/host/fonts/*.pgf)
    foreach(font ${ANYPSP_FALLBACK_FONTS})
        get_filename_component(name "${font}" NAME_WE)
        string(REPLACE "-" "_" name "${name}")
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/anypsp_font_${name}.cpp")
        add_custom_command(OUTPUT "${generated}"
            COMMAND "${CMAKE_COMMAND}" "-DIN=${font}" "-DOUT=${generated}" "-DNAME=anypsp_font_${name}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-file.cmake"
            DEPENDS "${font}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-file.cmake"
            COMMENT "Embedding runtime/host/fonts/${name}"
            VERBATIM)
        target_sources(anypsp_host PRIVATE "${generated}")
    endforeach()
    target_link_libraries(anypsp_host PUBLIC psprecomp_core anypsp_at3)
    target_compile_definitions(anypsp_host PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(anypsp_host PRIVATE /EHsc /permissive- /W3 /bigobj)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(anypsp_host PUBLIC Threads::Threads)

    # Interpreter-only runner: no translated code, the reference interpreter runs
    # everything. The oracle for the generated code and the conformance tests.
    add_executable(anypsp-interp runtime/host/main_headless.cpp runtime/host/no_generated.cpp
        runtime/host/no_modules.cpp runtime/host/no_dynamic_code.cpp)
    target_link_libraries(anypsp-interp PRIVATE anypsp_host anypsp_loader)
    if(MSVC)
        target_compile_options(anypsp-interp PRIVATE /EHsc /permissive- /W3)
        target_link_options(anypsp-interp PRIVATE /STACK:67108864)
    endif()

    # Per-library count of the HLE functions implemented, written after every build of the host.
    add_executable(anypsp-hle-coverage runtime/host/hle_coverage.cpp runtime/host/no_generated.cpp
        runtime/host/no_modules.cpp runtime/host/no_dynamic_code.cpp)
    target_link_libraries(anypsp-hle-coverage PRIVATE anypsp_host anypsp_loader)
    if(MSVC)
        target_compile_options(anypsp-hle-coverage PRIVATE /EHsc /permissive- /W3)
    endif()
    add_custom_command(TARGET anypsp-hle-coverage POST_BUILD
        COMMAND anypsp-hle-coverage --json "${CMAKE_BINARY_DIR}/hle-coverage.json" --markdown "${CMAKE_BINARY_DIR}/hle-coverage.md"
        COMMENT "Counting HLE functions per library (hle-coverage.json, hle-coverage.md)"
        VERBATIM)

    add_executable(anypsp_hle_tests tests/unit/main.cpp tests/unit/hle_tests.cpp tests/unit/hle_tests_kernel.cpp tests/unit/ge_texture_tests.cpp tests/unit/ge_interp_tests.cpp runtime/host/no_generated.cpp
        runtime/host/no_modules.cpp runtime/host/no_dynamic_code.cpp)
    target_include_directories(anypsp_hle_tests PRIVATE tests/unit)
    target_link_libraries(anypsp_hle_tests PRIVATE anypsp_host anypsp_loader)
    if(MSVC)
        target_compile_options(anypsp_hle_tests PRIVATE /EHsc /permissive- /W3)
    endif()
    add_test(NAME anypsp_hle_tests COMMAND anypsp_hle_tests)
endif()

# One runner per translated game: cmake -DANYPSP_RUNNER_NAME=<name> -DANYPSP_RUNNER_GENERATED=<psp_recomp --auto dir>,
# or several sharing one build of the host: -DANYPSP_RUNNERS="name=dir;name=dir".
set(ANYPSP_RUNNER_NAME "" CACHE STRING "Name of the headless runner to build")
set(ANYPSP_RUNNER_GENERATED "" CACHE PATH "Directory written by psp_recomp --auto for that runner")
set(ANYPSP_RUNNERS "" CACHE STRING "Runners to build, as name=generated-dir entries")
set(ANYPSP_RUNNER_OPT "1" CACHE STRING "Optimization level for the generated code: d (none, fastest build), 1 or 2")
set(ANYPSP_RUNNER_JOBS "8" CACHE STRING "Generated units compiled in parallel (MSVC /MP); each can take several GB")

function(anypsp_add_runner name generated)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${generated}/*.cpp")
    if(NOT sources)
        message(FATAL_ERROR "No generated code in ${generated}; run psp_recomp --auto first")
    endif()
    add_library(anypsp_generated_${name} STATIC ${sources})
    target_link_libraries(anypsp_generated_${name} PUBLIC psprecomp_core)
    if(MSVC)
        target_compile_options(anypsp_generated_${name} PRIVATE /bigobj /w /O${ANYPSP_RUNNER_OPT} /MP${ANYPSP_RUNNER_JOBS})
    else()
        target_compile_options(anypsp_generated_${name} PRIVATE -w -O$<IF:$<STREQUAL:${ANYPSP_RUNNER_OPT},d>,0,${ANYPSP_RUNNER_OPT}>)
    endif()
    add_executable(anypsp-run-${name} runtime/host/main_headless.cpp)
    if(NOT EXISTS "${generated}/modules_table.cpp")
        target_sources(anypsp-run-${name} PRIVATE runtime/host/no_modules.cpp)
    endif()
    if(NOT EXISTS "${generated}/dynamic_code.cpp")
        target_sources(anypsp-run-${name} PRIVATE runtime/host/no_dynamic_code.cpp)
    endif()
    target_link_libraries(anypsp-run-${name} PRIVATE anypsp_generated_${name} anypsp_host anypsp_loader)
    if(MSVC)
        target_compile_options(anypsp-run-${name} PRIVATE /EHsc /permissive- /W3)
        # Translated calls nest native frames (chained calls); 1 MiB is not enough.
        target_link_options(anypsp-run-${name} PRIVATE /STACK:67108864)
    endif()
endfunction()

if(ANYPSP_BUILD_HOST)
    if(NOT ANYPSP_RUNNER_NAME STREQUAL "")
        anypsp_add_runner(${ANYPSP_RUNNER_NAME} ${ANYPSP_RUNNER_GENERATED})
    endif()
    foreach(entry IN LISTS ANYPSP_RUNNERS)
        string(FIND "${entry}" "=" split)
        if(split LESS 1)
            message(FATAL_ERROR "ANYPSP_RUNNERS entry '${entry}' is not name=dir")
        endif()
        string(SUBSTRING "${entry}" 0 ${split} name)
        math(EXPR start "${split} + 1")
        string(SUBSTRING "${entry}" ${start} -1 generated)
        anypsp_add_runner(${name} ${generated})
    endforeach()
endif()

# Differential test of the code generator against the reference interpreter.
add_executable(anypsp-difftest-gen tests/cpu/difftest_gen.cpp)
target_link_libraries(anypsp-difftest-gen PRIVATE anypsp_codegen)
anypsp_warnings(anypsp-difftest-gen)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    COMMAND anypsp-difftest-gen ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    DEPENDS anypsp-difftest-gen)
add_executable(anypsp_cpu_difftest tests/cpu/difftest_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp)
target_include_directories(anypsp_cpu_difftest PRIVATE tests/cpu)
target_link_libraries(anypsp_cpu_difftest PRIVATE psprecomp_core)
if(MSVC)
    target_compile_options(anypsp_cpu_difftest PRIVATE /bigobj /EHsc)
endif()
add_test(NAME cpu_difftest COMMAND anypsp_cpu_difftest)

# Corpus check (skipped until python tests/fetch-corpus.py has run).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME corpus_analysis
             COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus-analysis.py
                     $<TARGET_FILE:anypsp-analyze> ${CMAKE_CURRENT_BINARY_DIR}/corpus-analysis)
    set_tests_properties(corpus_analysis PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Desktop GPU backend and its offscreen runners.
include(runtime/gpu/gpu.cmake)
