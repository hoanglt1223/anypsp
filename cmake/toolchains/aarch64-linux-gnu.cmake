# Linux arm64 (aarch64) cross build with clang and the GNU cross sysroot
# (Ubuntu/Debian: apt install g++-aarch64-linux-gnu, which provides /usr/aarch64-linux-gnu).
#
#   cmake -S . -B out/linux-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#   anypsp build game.iso --toolchain <AnyPSP source>/cmake/toolchains/aarch64-linux-gnu.cmake --target linux
#
# ANYPSP_AARCH64_SYSROOT overrides the sysroot (for example a handheld's root file system copied
# with its -dev packages, which also gives SDL3 its X11/Wayland/ALSA headers). Without those
# headers SDL3 builds with its KMS/DRM-free fallbacks only (offscreen and dummy drivers).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ANYPSP_AARCH64_TRIPLE aarch64-linux-gnu)
if(NOT ANYPSP_AARCH64_SYSROOT)
    set(ANYPSP_AARCH64_SYSROOT "$ENV{ANYPSP_AARCH64_SYSROOT}")
endif()
if(NOT ANYPSP_AARCH64_SYSROOT)
    set(ANYPSP_AARCH64_SYSROOT /usr/${ANYPSP_AARCH64_TRIPLE})
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ANYPSP_AARCH64_SYSROOT)

find_program(ANYPSP_CLANG clang REQUIRED)
find_program(ANYPSP_CLANGXX clang++ REQUIRED)
set(CMAKE_C_COMPILER ${ANYPSP_CLANG})
set(CMAKE_CXX_COMPILER ${ANYPSP_CLANGXX})
set(CMAKE_C_COMPILER_TARGET ${ANYPSP_AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${ANYPSP_AARCH64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${ANYPSP_AARCH64_TRIPLE})

# The GNU cross packages put headers and libraries under /usr/aarch64-linux-gnu and GCC's
# runtime under /usr/lib/gcc-cross; clang finds both with --gcc-toolchain=/usr.
if(EXISTS "${ANYPSP_AARCH64_SYSROOT}/include/c++")
    set(flags "--gcc-toolchain=/usr")
else()
    set(CMAKE_SYSROOT ${ANYPSP_AARCH64_SYSROOT})
    set(flags "")
endif()
set(CMAKE_C_FLAGS_INIT "${flags}")
set(CMAKE_CXX_FLAGS_INIT "${flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${flags}")

set(CMAKE_FIND_ROOT_PATH ${ANYPSP_AARCH64_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
