# Install layout of release packages (`cmake --install <build> --prefix <dir>`):
#
#   bin/anypsp, bin/anypsp-analyze, bin/psp_recomp      the pipeline `anypsp build` runs
#   bin/anypsp-play, bin/anypsp-interp                  interpreter players (no translation)
#   share/anypsp/nids.toml                              NID names for the installed programs
#   share/anypsp/source/                                the AnyPSP sources each game is compiled
#                                                       against, with SDL3 as third_party/SDL3-src.tar.gz
#
# The sources are shipped rather than prebuilt libraries because every game is one CMake build
# of the runtime with its generated code, for any target (desktop, Android, cross builds), with
# the user's own compiler. No game data, firmware or keys are installed.
include(GNUInstallDirs)

set(anypsp_programs anypsp-analyze psp_recomp)
foreach(program anypsp anypsp-play anypsp-interp)
    if(TARGET ${program})
        list(APPEND anypsp_programs ${program})
    endif()
endforeach()
install(TARGETS ${anypsp_programs} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/nids.toml DESTINATION ${CMAKE_INSTALL_DATADIR}/anypsp)

set(anypsp_source_dest ${CMAKE_INSTALL_DATADIR}/anypsp/source)
install(FILES
    ${PROJECT_SOURCE_DIR}/CMakeLists.txt ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/README.md
    ${PROJECT_SOURCE_DIR}/THIRD_PARTY.md
    DESTINATION ${anypsp_source_dest})
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/THIRD_PARTY.md DESTINATION .)
# tests/ is part of the CMake project (unit test targets); its fetched corpus is not.
foreach(dir cmake data platform recomp runtime tests tools)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/${dir} DESTINATION ${anypsp_source_dest}
        PATTERN corpus EXCLUDE PATTERN __pycache__ EXCLUDE PATTERN "*.pyc" EXCLUDE)
endforeach()

if(sdl3_SOURCE_DIR)
    # The SDL3 release the build fetched, packed so games build offline (tools/anypsp/build.cpp).
    install(CODE "
        set(archive \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${anypsp_source_dest}/third_party/SDL3-src.tar.gz\")
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${anypsp_source_dest}/third_party\")
        file(REMOVE_RECURSE \"${CMAKE_CURRENT_BINARY_DIR}/sdl3-pack\")
        file(COPY \"${sdl3_SOURCE_DIR}/\" DESTINATION \"${CMAKE_CURRENT_BINARY_DIR}/sdl3-pack/sdl3-src\"
             PATTERN .git EXCLUDE)
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E tar czf \"\${archive}\" sdl3-src
                        WORKING_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}/sdl3-pack\" RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR \"packing SDL3 failed\")
        endif()
        message(STATUS \"Installing: \${archive}\")
    ")
endif()
