#pragma once

#include <filesystem>

#include "frontend.h"
#include "indexer.h"
#include "storage.h"

namespace codegraph {


struct AffectedNodeView {
    NodeId node_id;
    std::string kind;
    std::string title;
};

int run_mcp_server(
    Storage& storage,
    FrontendRegistry& registry,
    const IndexOptions& options,
    const std::filesystem::path& codegraph_dir
);

}  // namespace codegraph
