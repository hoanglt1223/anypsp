# `anypsp build`: analyse, translate, compile against the AnyPSP sources and package (build.cpp),
# with APKs assembled without Gradle (android.cpp).
add_library(anypsp_driver_lib STATIC build.cpp android.cpp zip.cpp ${PROJECT_SOURCE_DIR}/runtime/gpu/disc.cpp)
target_include_directories(anypsp_driver_lib PUBLIC . ${PROJECT_SOURCE_DIR}/runtime/gpu)
target_link_libraries(anypsp_driver_lib PUBLIC psprecomp_core)
target_compile_definitions(anypsp_driver_lib PUBLIC ANYPSP_VERSION="${PROJECT_VERSION}"
    PRIVATE ANYPSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(sdl3_SOURCE_DIR)
    target_compile_definitions(anypsp_driver_lib PRIVATE ANYPSP_SDL3_SOURCE_DIR="${sdl3_SOURCE_DIR}")
endif()
anypsp_warnings(anypsp_driver_lib)

add_executable(anypsp main.cpp)
target_link_libraries(anypsp PRIVATE anypsp_driver_lib)
# Next to anypsp-analyze (and psp_recomp under recomp/), where it looks for them.
set_target_properties(anypsp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
anypsp_warnings(anypsp)

add_executable(anypsp_driver_tests ${PROJECT_SOURCE_DIR}/tests/unit/main.cpp ${PROJECT_SOURCE_DIR}/tests/unit/driver_tests.cpp)
target_include_directories(anypsp_driver_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
target_link_libraries(anypsp_driver_tests PRIVATE anypsp_driver_lib)
target_compile_definitions(anypsp_driver_tests PRIVATE ANYPSP_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME anypsp_driver_tests COMMAND anypsp_driver_tests)

# Coverage numbers for the compatibility site (tools/compat-site/generate.py --coverage).
if(TARGET anypsp_host)
    add_executable(anypsp-coverage coverage.cpp ${PROJECT_SOURCE_DIR}/runtime/host/no_generated.cpp
        ${PROJECT_SOURCE_DIR}/runtime/host/no_modules.cpp ${PROJECT_SOURCE_DIR}/runtime/host/no_dynamic_code.cpp)
    target_link_libraries(anypsp-coverage PRIVATE anypsp_host anypsp_loader anypsp_codegen)
    set_target_properties(anypsp-coverage PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
    if(MSVC)
        target_compile_options(anypsp-coverage PRIVATE /EHsc /permissive- /W3)
    endif()
endif()
