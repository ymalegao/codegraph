#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "core.h"
#include "storage.h"

namespace codegraph {

struct GraphIndex {
    Graph graph;
    StringInterner interner;
    std::vector<std::pair<uint64_t, NodeId>> symbol_by_namehash;
    std::vector<std::pair<StringId, FileId>> file_by_path;
};

GraphIndex build_graph_index(Storage& storage);

std::vector<NodeId> csr_neighbors(
    const Csr& csr,
    NodeId node,
    EdgeKind kind
);

std::vector<NodeId> graph_memory_for_node(
    const GraphIndex& index,
    NodeId node
);
uint32_t graph_memory_count_for_node(
    const GraphIndex& index,
    NodeId node
);

std::vector<NodeId> graph_symbols_by_name_hash(
    const GraphIndex& index,
    std::string_view name
);

}  // namespace codegraph
