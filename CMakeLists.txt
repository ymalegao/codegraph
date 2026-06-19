cmake_minimum_required(VERSION 3.20)

project(codegraph VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)

# cpp-tree-sitter wraps the tree-sitter runtime and provides add_grammar_from_repo.
CPMAddPackage(
  NAME cpp-tree-sitter
  GIT_REPOSITORY https://github.com/nsumner/cpp-tree-sitter.git
  GIT_TAG v0.0.1
)

# C++ grammar for tree-sitter.
# Important: add_grammar_from_repo expects the version WITHOUT the leading "v".
add_grammar_from_repo(tree-sitter-cpp
  https://github.com/tree-sitter/tree-sitter-cpp.git
  0.23.4
)

# Python grammar for tree-sitter.
# We only smoke-test this for now; full Python indexing is later.
add_grammar_from_repo(tree-sitter-python
  https://github.com/tree-sitter/tree-sitter-python.git
  0.20.4
)
include(FetchContent)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(SQLite3 REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)



add_library(codegraph_sanitizers INTERFACE)

target_compile_options(codegraph_sanitizers INTERFACE
  "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-fno-omit-frame-pointer;-O0;-g;-fsanitize=address,undefined>"
)

target_link_options(codegraph_sanitizers INTERFACE
  "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-fsanitize=address,undefined>"
)

add_library(codegraph_core
  src/core.cpp
)

target_include_directories(codegraph_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_core PUBLIC
  codegraph_sanitizers
)

target_compile_options(codegraph_core PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_storage
  src/storage.cpp
)

target_include_directories(codegraph_storage PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_storage PUBLIC
  codegraph_sanitizers
  SQLite::SQLite3
)

target_compile_options(codegraph_storage PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_scanner
  src/scanner.cpp
)

target_include_directories(codegraph_scanner PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_scanner PUBLIC
  codegraph_sanitizers
  codegraph_storage
  PkgConfig::XXHASH
)

target_compile_options(codegraph_scanner PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_executable(codegraph
  src/main.cpp
)

target_link_libraries(codegraph PRIVATE
  codegraph_core
  codegraph_storage
  codegraph_scanner
  codegraph_sanitizers
  cpp-tree-sitter
  tree-sitter-cpp
  tree-sitter-python
  nlohmann_json::nlohmann_json
  SQLite::SQLite3
  PkgConfig::XXHASH
)

target_compile_options(codegraph PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)
