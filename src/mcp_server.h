#pragma once

#include <filesystem>

#include "frontend.h"
#include "indexer.h"
#include "storage.h"

namespace codegraph {

int run_mcp_server(
    Storage& storage,
    FrontendRegistry& registry,
    const IndexOptions& options,
    const std::filesystem::path& codegraph_dir
);

}  // namespace codegraph
