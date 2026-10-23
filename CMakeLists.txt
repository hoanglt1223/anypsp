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
