cmake_minimum_required(VERSION 3.16)
project(raylib_lex_renamer_tool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# rlparser (optional helper)
add_executable(rlparser third_party/rlparser.c)

# main renamer
add_executable(raylib_renamer
    src/main.cpp
    src/lexer.cpp
    src/raylib_api_extract.cpp
    src/fs_utils.cpp
    src/consumer_heuristics.cpp
    src/multi_round_strict.cpp
)

target_include_directories(raylib_renamer PRIVATE src)

if (MSVC)
  target_compile_options(raylib_renamer PRIVATE /W4 /permissive-)
else()
  target_compile_options(raylib_renamer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------
# Tests (no external deps)
# -----------------
enable_testing()

add_executable(renamer_tests
    tests/test_basic.cpp
    src/lexer.cpp
    src/raylib_api_extract.cpp
    src/fs_utils.cpp
)

target_include_directories(renamer_tests PRIVATE src)

add_test(NAME renamer_tests COMMAND renamer_tests)
