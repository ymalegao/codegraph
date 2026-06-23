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
  src/file_util.cpp
  src/memory_kinds.cpp
  src/time_util.cpp
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
  src/sqlite_util.cpp
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

add_library(codegraph_hash
  src/hash_util.cpp
)

target_include_directories(codegraph_hash PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_hash PUBLIC
  codegraph_sanitizers
  PkgConfig::XXHASH
)

target_compile_options(codegraph_hash PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_graph_store
  src/graph_store.cpp
)

target_include_directories(codegraph_graph_store PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_graph_store PUBLIC
  codegraph_sanitizers
  codegraph_core
  codegraph_hash
  codegraph_source_projection
  codegraph_storage
)

target_compile_options(codegraph_graph_store PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_frontend
  src/frontend.cpp
)

target_include_directories(codegraph_frontend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_frontend PUBLIC
  codegraph_sanitizers
)

target_compile_options(codegraph_frontend PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_source_projection
  src/source_projection.cpp
)

target_include_directories(codegraph_source_projection PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_source_projection PUBLIC
  codegraph_sanitizers
  codegraph_storage
)

target_compile_options(codegraph_source_projection PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_resolver
  src/resolver.cpp
)

target_include_directories(codegraph_resolver PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_resolver PUBLIC
  codegraph_sanitizers
  codegraph_source_projection
  codegraph_storage
)

target_compile_options(codegraph_resolver PRIVATE
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
  codegraph_core
  codegraph_frontend
  codegraph_hash
  codegraph_resolver
  codegraph_source_projection
  codegraph_storage
)

target_compile_options(codegraph_scanner PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_indexer
  src/indexer.cpp
)

target_include_directories(codegraph_indexer PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_indexer PUBLIC
  codegraph_sanitizers
  codegraph_core
  codegraph_frontend
  codegraph_hash
  codegraph_source_projection
  codegraph_storage
)

target_compile_options(codegraph_indexer PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_cpp_frontend
  src/cpp_frontend.cpp
)

target_include_directories(codegraph_cpp_frontend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_cpp_frontend PUBLIC
  codegraph_sanitizers
  codegraph_frontend
  codegraph_hash
  cpp-tree-sitter
  tree-sitter-cpp
)

target_compile_options(codegraph_cpp_frontend PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_read_tools
  src/read_tools.cpp
)

target_include_directories(codegraph_read_tools PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_read_tools PUBLIC
  codegraph_sanitizers
  codegraph_frontend
  codegraph_hash
  codegraph_indexer
  codegraph_scanner
  codegraph_storage
)

target_compile_options(codegraph_read_tools PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_materializer
  src/materializer.cpp
)

target_include_directories(codegraph_materializer PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_materializer PUBLIC
  codegraph_sanitizers
  codegraph_core
  codegraph_hash
  codegraph_resolver
  codegraph_storage
  nlohmann_json::nlohmann_json
)

target_compile_options(codegraph_materializer PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_memory_reads
  src/memory_reads.cpp
)

target_include_directories(codegraph_memory_reads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_memory_reads PUBLIC
  codegraph_sanitizers
  codegraph_core
  codegraph_resolver
  codegraph_storage
)

target_compile_options(codegraph_memory_reads PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_bootstrap
  src/bootstrap.cpp
)

target_include_directories(codegraph_bootstrap PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_bootstrap PUBLIC
  codegraph_sanitizers
  codegraph_frontend
  codegraph_indexer
  codegraph_materializer
  codegraph_scanner
  codegraph_storage
)

target_compile_options(codegraph_bootstrap PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_library(codegraph_mcp_server
  src/mcp_server.cpp
)

target_include_directories(codegraph_mcp_server PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(codegraph_mcp_server PUBLIC
  codegraph_sanitizers
  codegraph_bootstrap
  codegraph_frontend
  codegraph_graph_store
  codegraph_hash
  codegraph_materializer
  codegraph_memory_reads
  codegraph_read_tools
  codegraph_storage
  nlohmann_json::nlohmann_json
)

target_compile_options(codegraph_mcp_server PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_executable(codegraph
  src/main.cpp
)

target_link_libraries(codegraph PRIVATE
  codegraph_core
  codegraph_bootstrap
  codegraph_cpp_frontend
  codegraph_frontend
  codegraph_graph_store
  codegraph_hash
  codegraph_indexer
  codegraph_materializer
  codegraph_mcp_server
  codegraph_memory_reads
  codegraph_read_tools
  codegraph_resolver
  codegraph_source_projection
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
