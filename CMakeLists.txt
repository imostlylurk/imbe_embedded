cmake_minimum_required(VERSION 3.13)
project(imbe-embedded C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

file(GLOB IMBE2_SRC src/*.cc)

add_library(imbe2 STATIC ${IMBE2_SRC})
target_include_directories(imbe2 PUBLIC inc)
# integer-only engine: optimization flags cannot change numerics
# -Wsign-conversion is deliberately off for the vendored fixed-point
# engine: Word16<->UWord16 / Word32<->UWord32 interchange IS the yazev
# idiom (sixty sites); the bit-exactness goldens are the real gate.
# Strict-aliasing puns are fixed via fft_datum (may_alias) and memset.
target_compile_options(imbe2 PRIVATE -O3 -Wall -Wextra -Wno-sign-conversion -flto=auto)
target_link_options(imbe2 INTERFACE -flto=auto)

add_executable(imbe_enc tools/imbe_enc.c)
add_executable(imbe_enc2 tools/imbe_enc2.c)
target_link_libraries(imbe_enc PRIVATE imbe2)
target_link_libraries(imbe_enc2 PRIVATE imbe2)

add_executable(imbe_dec tools/imbe_dec.c)
target_link_libraries(imbe_dec PRIVATE imbe2)

add_executable(imbe2test tests/imbe2test.c)
add_executable(imbebench tests/bench.c)
add_executable(irbench tests/irbench.c)
add_executable(mkgolden tools/mkgolden.c)
target_link_libraries(imbebench PRIVATE imbe2)
target_link_libraries(irbench PRIVATE imbe2)
target_link_libraries(imbe2test PRIVATE imbe2 m)
target_link_libraries(mkgolden PRIVATE imbe2)

enable_testing()
add_test(NAME imbe2test COMMAND imbe2test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
