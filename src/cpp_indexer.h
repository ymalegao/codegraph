#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "storage.h"

namespace codegraph {

struct IndexOptions {
    std::filesystem::path repo_root;
    std::string repo_id = "local";
};

struct IndexResult {
    uint32_t files_indexed = 0;
    uint32_t symbols_indexed = 0;
    uint32_t contains_edges = 0;
    uint32_t imports_edges = 0;
};

IndexResult index_cpp_repository(Storage& storage, const IndexOptions& options);

}  // namespace codegraph
