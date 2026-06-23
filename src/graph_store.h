#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core.h"
#include "storage.h"

namespace codegraph {

struct FileData {
    StringId path;
    StringId content_hash;
};

struct GraphSymbolView {
    NodeId node_id = NodeId::Invalid;
    FileId file = FileId{};
    SymbolKind kind = SymbolKind::Other;
    std::string_view name;
    std::string_view qualified_name;
    std::string_view signature;
    std::string_view path;
    std::string_view file_content_hash;
    SourceSpan span{};
};

struct GraphIndex {
    Graph graph;
    StringInterner interner;
    std::vector<FileData> files;
    std::vector<std::pair<uint64_t, NodeId>> symbol_by_namehash;
    std::vector<std::pair<StringId, FileId>> file_by_path;
    std::vector<std::pair<StringId, NodeId>> file_node_by_path;
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

std::optional<GraphSymbolView> graph_symbol(
    const GraphIndex& index,
    NodeId node
);

std::optional<FileData> graph_file(
    const GraphIndex& index,
    FileId file
);

std::optional<NodeId> graph_file_node_by_path(
    const GraphIndex& index,
    std::string_view path
);

// Recursive forward-Contains walk from a file node. Returns one
// {symbol_node, parent_node} pair for every symbol contained in the file,
// where parent_node is the file node for top-level symbols and the
// enclosing symbol node otherwise.
std::vector<std::pair<NodeId, NodeId>> graph_symbols_in_file(
    const GraphIndex& index,
    NodeId file_node
);

}  // namespace codegraph
