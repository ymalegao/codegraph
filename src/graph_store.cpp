#include "graph_store.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "hash_util.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

struct EdgeRow {
    uint32_t from = 0;
    uint32_t to = 0;
    EdgeKind kind = EdgeKind::Contains;
};

NodeKind parse_node_kind(std::string_view kind) {
    if (kind == "file") return NodeKind::File;
    if (kind == "symbol") return NodeKind::Symbol;
    if (kind == "correction") return NodeKind::Correction;
    if (kind == "arch_decision") return NodeKind::ArchDecision;
    throw std::runtime_error("unknown node kind: " + std::string(kind));
}

Status parse_status(std::string_view status) {
    if (status == "active") return Status::Active;
    if (status == "tombstoned") return Status::Tombstoned;
    if (status == "stale") return Status::Stale;
    return Status::Active;
}

SymbolKind parse_symbol_kind(std::string_view kind) {
    if (kind == "function") return SymbolKind::Function;
    if (kind == "method") return SymbolKind::Method;
    if (kind == "class") return SymbolKind::Class;
    if (kind == "struct") return SymbolKind::Struct;
    if (kind == "namespace") return SymbolKind::Namespace;
    if (kind == "enum") return SymbolKind::Enum;
    if (kind == "field") return SymbolKind::Field;
    return SymbolKind::Other;
}

EdgeKind parse_edge_kind(std::string_view kind) {
    if (kind == "contains") return EdgeKind::Contains;
    if (kind == "imports") return EdgeKind::Imports;
    if (kind == "affects") return EdgeKind::Affects;
    throw std::runtime_error("unknown edge kind: " + std::string(kind));
}

uint8_t parse_memory_type(std::string_view type) {
    if (type == "correction") return 0;
    if (type == "arch_decision") return 1;
    return 255;
}

uint32_t checked_node_index(int64_t value) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("node_id is outside uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

uint32_t checked_file_id(int64_t value) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("file_id is outside uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

uint64_t parse_hex_u64(std::string_view text) {
    uint64_t value = 0;
    for (const char ch : text) {
        value <<= 4U;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<uint64_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<uint64_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<uint64_t>(ch - 'A' + 10);
        } else {
            throw std::runtime_error("invalid hex hash");
        }
    }
    return value;
}

Csr build_csr(uint32_t node_count, const std::vector<EdgeRow>& edges, bool reverse) {
    Csr csr;
    csr.offsets.assign(static_cast<size_t>(node_count) + 1U, 0);

    for (const EdgeRow& edge : edges) {
        const uint32_t from = reverse ? edge.to : edge.from;
        if (from + 1U < csr.offsets.size()) {
            ++csr.offsets[from + 1U];
        }
    }

    for (size_t i = 1; i < csr.offsets.size(); ++i) {
        csr.offsets[i] += csr.offsets[i - 1U];
    }

    csr.neighbors.assign(csr.offsets.back(), NodeId::Invalid);
    csr.kinds.assign(csr.offsets.back(), EdgeKind::Contains);
    std::vector<uint32_t> cursor = csr.offsets;

    for (const EdgeRow& edge : edges) {
        const uint32_t from = reverse ? edge.to : edge.from;
        const uint32_t to = reverse ? edge.from : edge.to;
        if (from + 1U >= cursor.size()) {
            continue;
        }
        const uint32_t pos = cursor[from]++;
        csr.neighbors[pos] = static_cast<NodeId>(to);
        csr.kinds[pos] = edge.kind;
    }

    return csr;
}

void sort_unique_nodes(std::vector<NodeId>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](NodeId lhs, NodeId rhs) {
        return to_u32(lhs) < to_u32(rhs);
    });
    nodes.erase(
        std::unique(nodes.begin(), nodes.end(), [](NodeId lhs, NodeId rhs) {
            return lhs == rhs;
        }),
        nodes.end()
    );
}

}  // namespace

GraphIndex build_graph_index(Storage& storage) {
    GraphIndex index;
    const uint32_t max_node_id = checked_node_index(
        storage.query_int("SELECT COALESCE(MAX(node_id), 0) FROM nodes;")
    );
    index.graph.nodes.assign(
        static_cast<size_t>(max_node_id) + 1U,
        Node{NodeKind::File, Status::Tombstoned, 0, index.interner.intern(""), 0}
    );

    Statement nodes_stmt(
        storage.handle(),
        "SELECT node_id, kind, title, status FROM nodes ORDER BY node_id;"
    );
    while (nodes_stmt.step()) {
        const uint32_t node_id = checked_node_index(sqlite3_column_int64(nodes_stmt.get(), 0));
        index.graph.nodes[node_id] = Node{
            parse_node_kind(column_text(nodes_stmt.get(), 1)),
            parse_status(column_text(nodes_stmt.get(), 3)),
            0,
            index.interner.intern(column_text(nodes_stmt.get(), 2)),
            0,
        };
    }

    Statement file_stmt(
        storage.handle(),
        "SELECT file_id, path FROM files ORDER BY path;"
    );
    while (file_stmt.step()) {
        const auto file_id = static_cast<FileId>(checked_file_id(sqlite3_column_int64(file_stmt.get(), 0)));
        const StringId path = index.interner.intern(column_text(file_stmt.get(), 1));
        index.file_by_path.push_back({path, file_id});
    }
    std::sort(index.file_by_path.begin(), index.file_by_path.end());

    std::unordered_map<std::string, std::vector<uint32_t>> symbol_nodes_by_suffix;
    Statement symbol_node_stmt(
        storage.handle(),
        "SELECT node_id, stable_id FROM nodes WHERE kind = 'symbol';"
    );
    while (symbol_node_stmt.step()) {
        const std::string stable_id = column_text(symbol_node_stmt.get(), 1);
        static constexpr std::string_view kPrefix = "symbol:";
        if (stable_id.rfind(kPrefix, 0) != 0) {
            continue;
        }
        const size_t repo_end = stable_id.find(':', kPrefix.size());
        if (repo_end == std::string::npos) {
            continue;
        }
        symbol_nodes_by_suffix[stable_id.substr(repo_end)].push_back(
            checked_node_index(sqlite3_column_int64(symbol_node_stmt.get(), 0))
        );
    }

    Statement symbol_stmt(
        storage.handle(),
        "SELECT s.file_id, f.path, s.kind, s.name, s.qualified_name, COALESCE(s.signature, ''), "
        "s.start_line, s.end_line, s.start_byte, s.end_byte, s.content_hash "
        "FROM symbols s JOIN files f ON f.file_id = s.file_id "
        "ORDER BY f.path, s.qualified_name, s.start_line;"
    );
    while (symbol_stmt.step()) {
        const std::string path = column_text(symbol_stmt.get(), 1);
        const std::string qualified_name = column_text(symbol_stmt.get(), 4);
        const std::string suffix = ":" + path + "::" + qualified_name;
        const auto node_it = symbol_nodes_by_suffix.find(suffix);
        if (node_it == symbol_nodes_by_suffix.end() || node_it->second.empty()) {
            continue;
        }

        const uint32_t node_id = node_it->second.front();
        const uint32_t payload = static_cast<uint32_t>(index.graph.symbols.size());
        index.graph.nodes[node_id].payload = payload;
        index.graph.symbols.push_back(SymbolData{
            static_cast<FileId>(checked_file_id(sqlite3_column_int64(symbol_stmt.get(), 0))),
            parse_symbol_kind(column_text(symbol_stmt.get(), 2)),
            index.interner.intern(qualified_name),
            index.interner.intern(column_text(symbol_stmt.get(), 5)),
            SourceSpan{
                static_cast<uint32_t>(sqlite3_column_int64(symbol_stmt.get(), 6)),
                static_cast<uint32_t>(sqlite3_column_int64(symbol_stmt.get(), 7)),
                static_cast<uint32_t>(sqlite3_column_int64(symbol_stmt.get(), 8)),
                static_cast<uint32_t>(sqlite3_column_int64(symbol_stmt.get(), 9)),
                parse_hex_u64(column_text(symbol_stmt.get(), 10)),
                0,
            },
        });

        index.symbol_by_namehash.push_back({
            xxh64_u64(column_text(symbol_stmt.get(), 3)),
            static_cast<NodeId>(node_id),
        });
        index.symbol_by_namehash.push_back({
            xxh64_u64(qualified_name),
            static_cast<NodeId>(node_id),
        });
    }
    std::sort(index.symbol_by_namehash.begin(), index.symbol_by_namehash.end());

    Statement memo_stmt(
        storage.handle(),
        "SELECT m.node_id, m.memory_type, m.memory_id "
        "FROM memories m ORDER BY m.node_id;"
    );
    while (memo_stmt.step()) {
        const uint32_t node_id = checked_node_index(sqlite3_column_int64(memo_stmt.get(), 0));
        if (node_id >= index.graph.nodes.size()) {
            continue;
        }
        const uint32_t payload = static_cast<uint32_t>(index.graph.memos.size());
        index.graph.nodes[node_id].payload = payload;
        index.graph.memos.push_back(MemoryData{
            parse_memory_type(column_text(memo_stmt.get(), 1)),
            sqlite3_column_int64(memo_stmt.get(), 2),
        });
    }

    std::vector<EdgeRow> edges;
    Statement edge_stmt(
        storage.handle(),
        "SELECT from_node, to_node, kind FROM edges WHERE resolved = 1 AND to_node IS NOT NULL;"
    );
    while (edge_stmt.step()) {
        edges.push_back(EdgeRow{
            checked_node_index(sqlite3_column_int64(edge_stmt.get(), 0)),
            checked_node_index(sqlite3_column_int64(edge_stmt.get(), 1)),
            parse_edge_kind(column_text(edge_stmt.get(), 2)),
        });
    }

    index.graph.forward = build_csr(static_cast<uint32_t>(index.graph.nodes.size()), edges, false);
    index.graph.reverse = build_csr(static_cast<uint32_t>(index.graph.nodes.size()), edges, true);
    return index;
}

std::vector<NodeId> csr_neighbors(
    const Csr& csr,
    NodeId node,
    EdgeKind kind
) {
    const uint32_t index = to_u32(node);
    if (index + 1U >= csr.offsets.size()) {
        return {};
    }

    std::vector<NodeId> result;
    for (uint32_t i = csr.offsets[index]; i < csr.offsets[index + 1U]; ++i) {
        if (csr.kinds[i] == kind) {
            result.push_back(csr.neighbors[i]);
        }
    }
    return result;
}

std::vector<NodeId> graph_memory_for_node(
    const GraphIndex& index,
    NodeId node
) {
    std::vector<NodeId> memories;
    for (NodeId candidate : csr_neighbors(index.graph.reverse, node, EdgeKind::Affects)) {
        if (to_u32(candidate) < index.graph.nodes.size()) {
            const Node& memory = index.graph.nodes[to_u32(candidate)];
            if (memory.kind == NodeKind::Correction || memory.kind == NodeKind::ArchDecision) {
                memories.push_back(candidate);
            }
        }
    }
    sort_unique_nodes(memories);
    return memories;
}

uint32_t graph_memory_count_for_node(
    const GraphIndex& index,
    NodeId node
) {
    const uint32_t node_index = to_u32(node);
    if (node_index + 1U >= index.graph.reverse.offsets.size()) {
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = index.graph.reverse.offsets[node_index];
         i < index.graph.reverse.offsets[node_index + 1U];
         ++i) {
        if (index.graph.reverse.kinds[i] != EdgeKind::Affects) {
            continue;
        }
        const NodeId candidate = index.graph.reverse.neighbors[i];
        if (to_u32(candidate) < index.graph.nodes.size()) {
            const Node& memory = index.graph.nodes[to_u32(candidate)];
            if (memory.kind == NodeKind::Correction || memory.kind == NodeKind::ArchDecision) {
                ++count;
            }
        }
    }
    return count;
}

std::vector<NodeId> graph_symbols_by_name_hash(
    const GraphIndex& index,
    std::string_view name
) {
    const uint64_t hash = xxh64_u64(name);
    const auto begin = std::lower_bound(
        index.symbol_by_namehash.begin(),
        index.symbol_by_namehash.end(),
        hash,
        [](const auto& lhs, uint64_t rhs) {
            return lhs.first < rhs;
        }
    );
    const auto end = std::upper_bound(
        index.symbol_by_namehash.begin(),
        index.symbol_by_namehash.end(),
        hash,
        [](uint64_t lhs, const auto& rhs) {
            return lhs < rhs.first;
        }
    );

    std::vector<NodeId> result;
    for (auto it = begin; it != end; ++it) {
        result.push_back(it->second);
    }
    sort_unique_nodes(result);
    return result;
}

}  // namespace codegraph
