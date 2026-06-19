#include "source_projection.h"

#include "sqlite_util.h"

namespace codegraph {

namespace {
inline constexpr std::string_view kFileStablePrefix = "file:";
inline constexpr std::string_view kSymbolStablePrefix = "symbol:";
}  // namespace

std::string file_stable_id(std::string_view repo_id, std::string_view path) {
    return std::string(kFileStablePrefix) + std::string(repo_id) + ":" + std::string(path);
}

std::string symbol_stable_id(std::string_view repo_id, std::string_view path, std::string_view qualified_name) {
    return std::string(kSymbolStablePrefix) + std::string(repo_id) +
           symbol_stable_suffix(path, qualified_name);
}

std::string symbol_stable_prefix(std::string_view repo_id, std::string_view path) {
    return std::string(kSymbolStablePrefix) + std::string(repo_id) +
           symbol_stable_suffix(path, "") + "%";
}

std::string symbol_stable_suffix(std::string_view path, std::string_view qualified_name) {
    return ":" + std::string(path) + "::" + std::string(qualified_name);
}

bool parse_symbol_stable_id(std::string_view stable_id, SymbolStableIdParts& parts) {
    if (stable_id.rfind(kSymbolStablePrefix, 0) != 0) {
        return false;
    }

    const size_t repo_start = kSymbolStablePrefix.size();
    const size_t repo_end = stable_id.find(':', repo_start);
    if (repo_end == std::string_view::npos) {
        return false;
    }

    const size_t path_start = repo_end + 1U;
    const size_t path_end = stable_id.find("::", path_start);
    if (path_end == std::string_view::npos) {
        return false;
    }

    parts.repo_id = std::string(stable_id.substr(repo_start, repo_end - repo_start));
    parts.path = std::string(stable_id.substr(path_start, path_end - path_start));
    parts.qualified_name = std::string(stable_id.substr(path_end + 2U));
    return !parts.repo_id.empty() && !parts.path.empty() && !parts.qualified_name.empty();
}

void delete_source_file_projection(
    sqlite3* db,
    std::string_view repo_id,
    int64_t file_id,
    std::string_view path,
    bool delete_file_row
) {
    const std::string file_stable = file_stable_id(repo_id, path);
    const std::string symbol_prefix = symbol_stable_prefix(repo_id, path);
    int64_t file_node_id = -1;

    Statement select_file_node(db, "SELECT node_id FROM nodes WHERE stable_id = ?;");
    bind_text(select_file_node.get(), 1, file_stable);
    if (select_file_node.step()) {
        file_node_id = sqlite3_column_int64(select_file_node.get(), 0);
    }

    Statement reset_affects(
        db,
        "UPDATE edges SET to_node = NULL, resolved = 0 "
        "WHERE kind = 'affects' "
        "AND (to_node = ? OR to_node IN (SELECT node_id FROM nodes WHERE stable_id LIKE ?));"
    );
    bind_int64(reset_affects.get(), 1, file_node_id);
    bind_text(reset_affects.get(), 2, symbol_prefix);
    reset_affects.expect_done("reset memory edges to deleted source nodes");

    Statement delete_edges(
        db,
        "DELETE FROM edges "
        "WHERE kind IN ('contains', 'imports') "
        "AND (from_node = ? OR to_node = ? "
        "OR from_node IN (SELECT node_id FROM nodes WHERE stable_id LIKE ?) "
        "OR to_node IN (SELECT node_id FROM nodes WHERE stable_id LIKE ?));"
    );
    bind_int64(delete_edges.get(), 1, file_node_id);
    bind_int64(delete_edges.get(), 2, file_node_id);
    bind_text(delete_edges.get(), 3, symbol_prefix);
    bind_text(delete_edges.get(), 4, symbol_prefix);
    delete_edges.expect_done("delete old source edges");

    Statement delete_symbols(db, "DELETE FROM symbols WHERE file_id = ?;");
    bind_int64(delete_symbols.get(), 1, file_id);
    delete_symbols.expect_done("delete old symbols");

    Statement delete_symbol_nodes(db, "DELETE FROM nodes WHERE stable_id LIKE ?;");
    bind_text(delete_symbol_nodes.get(), 1, symbol_prefix);
    delete_symbol_nodes.expect_done("delete old symbol nodes");

    if (delete_file_row) {
        Statement delete_line_table(db, "DELETE FROM line_tables WHERE file_id = ?;");
        bind_int64(delete_line_table.get(), 1, file_id);
        delete_line_table.expect_done("delete stale line table");

        Statement delete_file_node(db, "DELETE FROM nodes WHERE stable_id = ?;");
        bind_text(delete_file_node.get(), 1, file_stable);
        delete_file_node.expect_done("delete stale file node");

        Statement delete_file(db, "DELETE FROM files WHERE file_id = ?;");
        bind_int64(delete_file.get(), 1, file_id);
        delete_file.expect_done("delete stale file row");
    }
}

}  // namespace codegraph
