#include "resolver.h"

#include <string>
#include <vector>

#include <sqlite3.h>

#include "sqlite_util.h"

namespace codegraph {
namespace {

int64_t resolve_file(Storage& storage, std::string_view path) {
    Statement stmt(
        storage.handle(),
        "SELECT n.node_id "
        "FROM files f JOIN nodes n ON n.kind = 'file' "
        "AND n.stable_id LIKE ('file:%:' || f.path) "
        "WHERE f.path = ? "
        "ORDER BY n.node_id LIMIT 1;"
    );
    bind_text(stmt.get(), 1, path);
    if (!stmt.step()) {
        return -1;
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

int64_t resolve_symbol_in_file(
    Storage& storage,
    std::string_view path,
    std::string_view qualified_name
) {
    Statement stmt(
        storage.handle(),
        "SELECT sn.node_id "
        "FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "JOIN nodes sn ON sn.kind = 'symbol' "
        "AND sn.stable_id LIKE ('symbol:%:' || f.path || '::' || s.qualified_name) "
        "WHERE f.path = ? AND s.qualified_name = ? "
        "ORDER BY sn.node_id LIMIT 2;"
    );
    bind_text(stmt.get(), 1, path);
    bind_text(stmt.get(), 2, qualified_name);

    if (!stmt.step()) {
        return -1;
    }
    const int64_t node_id = sqlite3_column_int64(stmt.get(), 0);
    if (stmt.step()) {
        return resolve_file(storage, path);
    }
    return node_id;
}

int64_t resolve_unique_symbol(Storage& storage, std::string_view qualified_name) {
    Statement stmt(
        storage.handle(),
        "SELECT sn.node_id "
        "FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "JOIN nodes sn ON sn.kind = 'symbol' "
        "AND sn.stable_id LIKE ('symbol:%:' || f.path || '::' || s.qualified_name) "
        "WHERE s.qualified_name = ? "
        "ORDER BY sn.node_id LIMIT 2;"
    );
    bind_text(stmt.get(), 1, qualified_name);

    if (!stmt.step()) {
        return -1;
    }
    const int64_t node_id = sqlite3_column_int64(stmt.get(), 0);
    if (stmt.step()) {
        return -1;
    }
    return node_id;
}

}  // namespace

int64_t resolve_reference(Storage& storage, std::string_view ref) {
    const size_t separator = ref.find("::");
    if (separator != std::string_view::npos) {
        const std::string_view path = ref.substr(0, separator);
        const std::string_view qualified_name = ref.substr(separator + 2U);
        const int64_t symbol_node = resolve_symbol_in_file(storage, path, qualified_name);
        if (symbol_node >= 0) {
            return symbol_node;
        }
    }

    const int64_t file_node = resolve_file(storage, ref);
    if (file_node >= 0) {
        return file_node;
    }

    return resolve_unique_symbol(storage, ref);
}

uint32_t resolver_pass(Storage& storage) {
    Statement select_edges(
        storage.handle(),
        "SELECT edge_id, to_ref FROM edges "
        "WHERE resolved = 0 AND to_ref IS NOT NULL AND to_ref != '' "
        "ORDER BY edge_id;"
    );

    struct PendingEdge {
        int64_t edge_id = 0;
        std::string to_ref;
    };
    std::vector<PendingEdge> pending;
    while (select_edges.step()) {
        const auto* to_ref = reinterpret_cast<const char*>(sqlite3_column_text(select_edges.get(), 1));
        pending.push_back(PendingEdge{
            sqlite3_column_int64(select_edges.get(), 0),
            to_ref == nullptr ? "" : to_ref,
        });
    }

    Statement update_edge(
        storage.handle(),
        "UPDATE edges SET to_node = ?, resolved = 1 WHERE edge_id = ?;"
    );

    uint32_t resolved = 0;
    for (const PendingEdge& edge : pending) {
        const int64_t target = resolve_reference(storage, edge.to_ref);
        if (target < 0) {
            continue;
        }

        update_edge.reset();
        bind_int64(update_edge.get(), 1, target);
        bind_int64(update_edge.get(), 2, edge.edge_id);
        update_edge.expect_done("resolve pending edge");
        ++resolved;
    }

    return resolved;
}

}  // namespace codegraph
