#include "indexer.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "file_util.h"
#include "source_projection.h"
#include "sqlite_util.h"
#include "time_util.h"

namespace codegraph {
namespace {

struct FileRow {
    int64_t file_id = 0;
    std::string path;
    std::string content_hash;
    std::string commit_hash;
};

struct IndexedSymbol {
    SymbolInfo info;
    int64_t node_id = 0;
};

std::vector<FileRow> load_files_for_language(sqlite3* db, std::string_view language) {
    Statement stmt(
        db,
        "SELECT file_id, path, content_hash, COALESCE(commit_hash, '') "
        "FROM files WHERE language = ? ORDER BY path;"
    );
    bind_text(stmt.get(), 1, language);

    std::vector<FileRow> files;
    while (stmt.step()) {
        files.push_back(FileRow{
            sqlite3_column_int64(stmt.get(), 0),
            column_text(stmt.get(), 1),
            column_text(stmt.get(), 2),
            column_text(stmt.get(), 3),
        });
    }
    return files;
}

int64_t upsert_source_node(
    Statement& upsert_node_stmt,
    Statement& select_node_stmt,
    std::string_view stable_id,
    std::string_view kind,
    std::string_view title
) {
    const std::string created_at = current_utc_timestamp();

    upsert_node_stmt.reset();
    bind_text(upsert_node_stmt.get(), 1, stable_id);
    bind_text(upsert_node_stmt.get(), 2, kind);
    bind_text(upsert_node_stmt.get(), 3, title);
    bind_text(upsert_node_stmt.get(), 4, created_at);
    bind_text(upsert_node_stmt.get(), 5, status_text(Status::Active));
    upsert_node_stmt.expect_done("upsert source node");

    select_node_stmt.reset();
    bind_text(select_node_stmt.get(), 1, stable_id);
    select_node_stmt.expect_row("select source node");
    return sqlite3_column_int64(select_node_stmt.get(), 0);
}

int64_t insert_symbol_row(sqlite3* db, Statement& insert_symbol_stmt, const FileRow& file, const SymbolInfo& symbol) {
    insert_symbol_stmt.reset();
    bind_int64(insert_symbol_stmt.get(), 1, file.file_id);
    bind_text(insert_symbol_stmt.get(), 2, symbol_kind_text(symbol_kind_from_string(symbol.kind)));
    bind_text(insert_symbol_stmt.get(), 3, symbol.name);
    bind_text(insert_symbol_stmt.get(), 4, symbol.qualified_name);
    bind_text(insert_symbol_stmt.get(), 5, symbol.signature);
    bind_int64(insert_symbol_stmt.get(), 6, symbol.start_line);
    bind_int64(insert_symbol_stmt.get(), 7, symbol.end_line);
    bind_int64(insert_symbol_stmt.get(), 8, symbol.start_byte);
    bind_int64(insert_symbol_stmt.get(), 9, symbol.end_byte);
    bind_text(insert_symbol_stmt.get(), 10, symbol.content_hash);
    bind_text(insert_symbol_stmt.get(), 11, file.content_hash);
    insert_symbol_stmt.expect_done("insert symbol");
    return sqlite3_last_insert_rowid(db);
}

void insert_edge_row(
    Statement& insert_edge_stmt,
    int64_t from_node,
    int64_t to_node,
    std::string_view to_ref,
    std::string_view kind,
    bool resolved
) {
    insert_edge_stmt.reset();
    bind_int64(insert_edge_stmt.get(), 1, from_node);
    if (to_node >= 0) {
        bind_int64(insert_edge_stmt.get(), 2, to_node);
    } else {
        check_sqlite(
            sqlite3_bind_null(insert_edge_stmt.get(), 2),
            sqlite3_db_handle(insert_edge_stmt.get()),
            "bind null edge target"
        );
    }
    bind_text(insert_edge_stmt.get(), 3, to_ref);
    bind_text(insert_edge_stmt.get(), 4, kind);
    bind_int64(insert_edge_stmt.get(), 5, resolved ? 1 : 0);
    insert_edge_stmt.expect_done("insert edge");
}

bool source_projection_is_current(sqlite3* db, const FileRow& file) {
    Statement stmt(
        db,
        "SELECT COUNT(*) FROM symbols WHERE file_id = ? AND commit_hash = ?;"
    );
    bind_int64(stmt.get(), 1, file.file_id);
    bind_text(stmt.get(), 2, file.content_hash);
    stmt.expect_row("count current symbols");
    return sqlite3_column_int64(stmt.get(), 0) > 0;
}

}  // namespace

IndexResult index_repository(
    Storage& storage,
    const FrontendRegistry& registry,
    const IndexOptions& options
) {
    const std::filesystem::path repo_root = std::filesystem::weakly_canonical(options.repo_root);

    Statement upsert_node(
        storage.handle(),
        "INSERT INTO nodes(stable_id, kind, title, created_at, status) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(stable_id) DO UPDATE SET "
        "kind = excluded.kind, title = excluded.title, status = excluded.status;"
    );
    Statement select_node(storage.handle(), "SELECT node_id FROM nodes WHERE stable_id = ?;");
    Statement insert_symbol(
        storage.handle(),
        "INSERT INTO symbols(file_id, kind, name, qualified_name, signature, "
        "start_line, end_line, start_byte, end_byte, content_hash, commit_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
    );
    Statement insert_edge(
        storage.handle(),
        "INSERT INTO edges(from_node, to_node, to_ref, kind, resolved) "
        "VALUES (?, ?, ?, ?, ?);"
    );

    IndexResult result;
    storage.execute("BEGIN IMMEDIATE;");
    try {
        for (const auto& frontend : registry.all()) {
            const std::vector<FileRow> files = load_files_for_language(
                storage.handle(),
                frontend->language()
            );

            for (const FileRow& file : files) {
                const std::filesystem::path abs_path = repo_root / file.path;
                if (!std::filesystem::is_regular_file(abs_path)) {
                    delete_source_file_projection(
                        storage.handle(),
                        options.repo_id,
                        file.file_id,
                        file.path,
                        true
                    );
                    ++result.files_pruned;
                    continue;
                }

                if (source_projection_is_current(storage.handle(), file)) {
                    ++result.files_unchanged;
                    continue;
                }

                const std::string source = read_file_bytes(abs_path);
                const ExtractedFile extracted = frontend->extract(source);

                const std::string file_stable = file_stable_id(options.repo_id, file.path);
                const int64_t file_node_id = upsert_source_node(
                    upsert_node,
                    select_node,
                    file_stable,
                    node_kind_text(NodeKind::File),
                    file.path
                );

                delete_source_file_projection(
                    storage.handle(),
                    options.repo_id,
                    file.file_id,
                    file.path,
                    false
                );

                std::vector<IndexedSymbol> symbols;
                symbols.reserve(extracted.symbols.size());
                for (const SymbolInfo& symbol : extracted.symbols) {
                    (void)insert_symbol_row(storage.handle(), insert_symbol, file, symbol);
                    const std::string stable =
                        symbol_stable_id(options.repo_id, file.path, symbol.qualified_name);
                    const int64_t symbol_node_id = upsert_source_node(
                        upsert_node,
                        select_node,
                        stable,
                        node_kind_text(NodeKind::Symbol),
                        symbol.qualified_name
                    );
                    symbols.push_back(IndexedSymbol{symbol, symbol_node_id});
                    ++result.symbols_indexed;
                }

                for (size_t i = 0; i < symbols.size(); ++i) {
                    const SymbolInfo& symbol = symbols[i].info;
                    const int64_t parent_node =
                        symbol.parent_index >= 0
                            ? symbols[static_cast<size_t>(symbol.parent_index)].node_id
                            : file_node_id;
                    insert_edge_row(
                        insert_edge,
                        parent_node,
                        symbols[i].node_id,
                        "",
                        edge_kind_text(EdgeKind::Contains),
                        true
                    );
                    ++result.contains_edges;
                }

                for (const IncludeInfo& include : extracted.includes) {
                    insert_edge_row(
                        insert_edge,
                        file_node_id,
                        -1,
                        include.target,
                        edge_kind_text(EdgeKind::Imports),
                        false
                    );
                    ++result.imports_edges;
                }

                ++result.files_indexed;
            }
        }

        storage.execute("COMMIT;");
    } catch (...) {
        storage.execute("ROLLBACK;");
        throw;
    }

    return result;
}

}  // namespace codegraph
