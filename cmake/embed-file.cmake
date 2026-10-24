# Writes a C++ source holding a file's bytes:
#   const unsigned char <NAME>[]; const std::size_t <NAME>_size;
# cmake -DIN=<file> -DOUT=<file.cpp> -DNAME=<symbol> -P embed-file.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],))" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}.tmp" "// Generated from ${IN} by cmake/embed-file.cmake.\n#include <cstddef>\n"
  "extern const unsigned char ${NAME}[] = {\n${bytes}0};\nextern const std::size_t ${NAME}_size = ${size};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
