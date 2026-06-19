#include "resolver.h"

#include <string>
#include <vector>

#include <sqlite3.h>

#include "source_projection.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

int64_t resolve_file(Storage& storage, std::string_view path) {
    Statement stmt(
        storage.handle(),
        "SELECT n.node_id "
        "FROM nodes n "
        "WHERE n.kind = 'file' AND n.title = ? "
        "ORDER BY n.node_id LIMIT 1;"
    );
    bind_text(stmt.get(), 1, path);
    if (!stmt.step()) {
        return -1;
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

std::vector<std::string> repo_ids_for_file(Storage& storage, std::string_view path) {
    Statement stmt(
        storage.handle(),
        "SELECT stable_id FROM nodes WHERE kind = 'file' AND title = ? ORDER BY node_id;"
    );
    bind_text(stmt.get(), 1, path);

    std::vector<std::string> repo_ids;
    const std::string suffix = ":" + std::string(path);
    while (stmt.step()) {
        const std::string stable_id = column_text(stmt.get(), 0);
        static constexpr std::string_view kPrefix = "file:";
        if (stable_id.rfind(kPrefix, 0) != 0 || stable_id.size() <= kPrefix.size() + suffix.size()) {
            continue;
        }
        if (stable_id.substr(stable_id.size() - suffix.size()) != suffix) {
            continue;
        }
        repo_ids.push_back(stable_id.substr(kPrefix.size(), stable_id.size() - kPrefix.size() - suffix.size()));
    }
    return repo_ids;
}

int64_t resolve_symbol_in_file(
    Storage& storage,
    std::string_view path,
    std::string_view qualified_name
) {
    const std::vector<std::string> repo_ids = repo_ids_for_file(storage, path);
    if (repo_ids.empty()) {
        return -1;
    }

    Statement stmt(
        storage.handle(),
        "SELECT node_id FROM nodes WHERE stable_id = ? AND kind = 'symbol';"
    );

    int64_t found = -1;
    for (const std::string& repo_id : repo_ids) {
        stmt.reset();
        bind_text(stmt.get(), 1, symbol_stable_id(repo_id, path, qualified_name));
        if (!stmt.step()) {
            continue;
        }
        if (found >= 0) {
            return resolve_file(storage, path);
        }
        found = sqlite3_column_int64(stmt.get(), 0);
    }
    return found;
}

int64_t resolve_unique_symbol(Storage& storage, std::string_view qualified_name) {
    Statement stmt(
        storage.handle(),
        "SELECT n.node_id "
        "FROM symbols s JOIN nodes n ON n.kind = 'symbol' AND n.title = s.qualified_name "
        "WHERE s.qualified_name = ? "
        "ORDER BY n.node_id LIMIT 2;"
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
        pending.push_back(PendingEdge{
            sqlite3_column_int64(select_edges.get(), 0),
            column_text(select_edges.get(), 1),
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
