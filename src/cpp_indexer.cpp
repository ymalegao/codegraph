#include "cpp_indexer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "file_util.h"
#include "hash_util.h"
#include "sqlite_util.h"
#include "time_util.h"

extern "C" {
const TSLanguage* tree_sitter_cpp();
}

namespace codegraph {
namespace {

struct FileRow {
    int64_t file_id = 0;
    std::string path;
    std::string commit_hash;
};

struct SymbolInfo {
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;
    std::string content_hash;
    int parent_index = -1;
    int64_t node_id = 0;
};

struct IncludeInfo {
    std::string target;
};

struct ExtractedFile {
    std::vector<SymbolInfo> symbols;
    std::vector<IncludeInfo> includes;
};

class Parser {
public:
    Parser() : parser_(ts_parser_new()) {
        if (parser_ == nullptr) {
            throw std::runtime_error("failed to allocate tree-sitter parser");
        }
        if (!ts_parser_set_language(parser_, tree_sitter_cpp())) {
            throw std::runtime_error("failed to set tree-sitter C++ language");
        }
    }

    ~Parser() {
        ts_parser_delete(parser_);
    }

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    TSTree* parse(std::string_view source) {
        TSTree* tree = ts_parser_parse_string(
            parser_,
            nullptr,
            source.data(),
            static_cast<uint32_t>(source.size())
        );
        if (tree == nullptr) {
            throw std::runtime_error("tree-sitter C++ parse failed");
        }
        return tree;
    }

private:
    TSParser* parser_ = nullptr;
};

class Tree {
public:
    explicit Tree(TSTree* tree) : tree_(tree) {}
    ~Tree() {
        ts_tree_delete(tree_);
    }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    TSNode root() const {
        return ts_tree_root_node(tree_);
    }

private:
    TSTree* tree_ = nullptr;
};

TSNode child_by_field(TSNode node, const char* field) {
    return ts_node_child_by_field_name(node, field, static_cast<uint32_t>(std::strlen(field)));
}

std::string node_text(std::string_view source, TSNode node) {
    if (ts_node_is_null(node)) {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start > end || end > source.size()) {
        throw std::runtime_error("tree-sitter node byte range is outside source");
    }
    return std::string(source.substr(start, end - start));
}

std::string_view node_type(TSNode node) {
    return ts_node_type(node);
}

std::string join_qualified(const std::vector<std::string>& scope, std::string_view name) {
    if (scope.empty()) {
        return std::string(name);
    }

    std::string qualified;
    for (const std::string& part : scope) {
        if (!qualified.empty()) {
            qualified += "::";
        }
        qualified += part;
    }
    const std::string name_text(name);
    const std::string qualified_prefix = qualified + "::";
    if (name_text == qualified || name_text.rfind(qualified_prefix, 0) == 0) {
        return name_text;
    }

    qualified += "::";
    qualified += name_text;
    return qualified;
}

std::string unqualified_name(std::string name) {
    const size_t pos = name.rfind("::");
    if (pos != std::string::npos) {
        return name.substr(pos + 2U);
    }
    return name;
}

bool is_identifier_like(std::string_view type) {
    return type == "identifier" ||
           type == "field_identifier" ||
           type == "type_identifier" ||
           type == "namespace_identifier" ||
           type == "qualified_identifier" ||
           type == "destructor_name" ||
           type == "operator_name";
}

TSNode find_declarator_name(TSNode node) {
    if (ts_node_is_null(node)) {
        return node;
    }

    if (is_identifier_like(node_type(node))) {
        return node;
    }

    TSNode declarator = child_by_field(node, "declarator");
    if (!ts_node_is_null(declarator)) {
        TSNode nested = find_declarator_name(declarator);
        if (!ts_node_is_null(nested)) {
            return nested;
        }
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode nested = find_declarator_name(ts_node_named_child(node, i));
        if (!ts_node_is_null(nested)) {
            return nested;
        }
    }

    return TSNode{};
}

std::string symbol_kind_for_type(std::string_view type) {
    if (type == "class_specifier") return "class";
    if (type == "struct_specifier") return "struct";
    if (type == "namespace_definition") return "namespace";
    if (type == "enum_specifier") return "enum";
    return "other";
}

std::string strip_include_delimiters(std::string text) {
    if (text.size() >= 2U) {
        const char first = text.front();
        const char last = text.back();
        if ((first == '<' && last == '>') || (first == '"' && last == '"')) {
            return text.substr(1U, text.size() - 2U);
        }
    }
    return text;
}

void collect_includes(TSNode node, std::string_view source, std::vector<IncludeInfo>& includes) {
    if (node_type(node) == "preproc_include") {
        TSNode path = child_by_field(node, "path");
        if (!ts_node_is_null(path)) {
            includes.push_back(IncludeInfo{strip_include_delimiters(node_text(source, path))});
        }
        return;
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collect_includes(ts_node_named_child(node, i), source, includes);
    }
}

void collect_symbols(
    TSNode node,
    std::string_view source,
    std::vector<std::string>& scope,
    std::vector<int>& parent_stack,
    int type_depth,
    std::vector<SymbolInfo>& symbols
) {
    const std::string_view type = node_type(node);

    if (type == "namespace_definition" ||
        type == "class_specifier" ||
        type == "struct_specifier" ||
        type == "enum_specifier") {
        TSNode name_node = child_by_field(node, "name");
        if (!ts_node_is_null(name_node)) {
            const std::string name = node_text(source, name_node);
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            const uint32_t start_byte = ts_node_start_byte(node);
            const uint32_t end_byte = ts_node_end_byte(node);

            SymbolInfo symbol;
            symbol.kind = symbol_kind_for_type(type);
            symbol.name = unqualified_name(name);
            symbol.qualified_name = join_qualified(scope, name);
            symbol.start_line = start.row + 1U;
            symbol.end_line = end.row + 1U;
            symbol.start_byte = start_byte;
            symbol.end_byte = end_byte;
            symbol.content_hash = xxh64_hex(source.substr(start_byte, end_byte - start_byte));
            symbol.parent_index = parent_stack.empty() ? -1 : parent_stack.back();

            const int current_index = static_cast<int>(symbols.size());
            symbols.push_back(std::move(symbol));

            scope.push_back(name);
            parent_stack.push_back(current_index);
            const int next_type_depth = type == "namespace_definition" ? type_depth : type_depth + 1;
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                collect_symbols(
                    ts_node_named_child(node, i),
                    source,
                    scope,
                    parent_stack,
                    next_type_depth,
                    symbols
                );
            }
            parent_stack.pop_back();
            scope.pop_back();
        }
        return;
    }

    if (type == "function_definition") {
        TSNode declarator = child_by_field(node, "declarator");
        TSNode name_node = find_declarator_name(declarator);
        if (!ts_node_is_null(name_node)) {
            const std::string raw_name = node_text(source, name_node);
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            const uint32_t start_byte = ts_node_start_byte(node);
            const uint32_t end_byte = ts_node_end_byte(node);

            SymbolInfo symbol;
            symbol.kind = type_depth > 0 || raw_name.find("::") != std::string::npos ? "method" : "function";
            symbol.name = unqualified_name(raw_name);
            symbol.qualified_name = join_qualified(scope, raw_name);
            symbol.signature = node_text(source, declarator);
            symbol.start_line = start.row + 1U;
            symbol.end_line = end.row + 1U;
            symbol.start_byte = start_byte;
            symbol.end_byte = end_byte;
            symbol.content_hash = xxh64_hex(source.substr(start_byte, end_byte - start_byte));
            symbol.parent_index = parent_stack.empty() ? -1 : parent_stack.back();
            symbols.push_back(std::move(symbol));
        }
        return;
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collect_symbols(ts_node_named_child(node, i), source, scope, parent_stack, type_depth, symbols);
    }
}

ExtractedFile extract_cpp_symbols(std::string_view source) {
    Parser parser;
    Tree tree(parser.parse(source));

    TSNode root = tree.root();
    if (ts_node_has_error(root)) {
        throw std::runtime_error("tree-sitter C++ parse produced errors");
    }

    ExtractedFile extracted;
    collect_includes(root, source, extracted.includes);

    std::vector<std::string> scope;
    std::vector<int> parent_stack;
    collect_symbols(root, source, scope, parent_stack, 0, extracted.symbols);

    return extracted;
}

std::vector<FileRow> load_cpp_files(sqlite3* db) {
    Statement stmt(
        db,
        "SELECT file_id, path, COALESCE(commit_hash, '') "
        "FROM files WHERE language = 'cpp' ORDER BY path;"
    );

    std::vector<FileRow> files;
    while (stmt.step()) {
        const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const auto* commit = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        files.push_back(FileRow{
            sqlite3_column_int64(stmt.get(), 0),
            path == nullptr ? "" : path,
            commit == nullptr ? "" : commit,
        });
    }
    return files;
}

std::string file_stable_id(std::string_view repo_id, std::string_view path) {
    return "file:" + std::string(repo_id) + ":" + std::string(path);
}

std::string symbol_stable_id(std::string_view repo_id, std::string_view path, std::string_view qualified_name) {
    return "symbol:" + std::string(repo_id) + ":" + std::string(path) + "::" + std::string(qualified_name);
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
    upsert_node_stmt.expect_done("upsert source node");

    select_node_stmt.reset();
    bind_text(select_node_stmt.get(), 1, stable_id);
    select_node_stmt.expect_row("select source node");
    return sqlite3_column_int64(select_node_stmt.get(), 0);
}

void delete_file_projection(
    sqlite3* db,
    int64_t file_id,
    int64_t file_node_id,
    std::string_view symbol_stable_prefix
) {
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
    bind_text(delete_edges.get(), 3, symbol_stable_prefix);
    bind_text(delete_edges.get(), 4, symbol_stable_prefix);
    delete_edges.expect_done("delete old source edges");

    Statement delete_symbols(db, "DELETE FROM symbols WHERE file_id = ?;");
    bind_int64(delete_symbols.get(), 1, file_id);
    delete_symbols.expect_done("delete old symbols");

    Statement delete_symbol_nodes(db, "DELETE FROM nodes WHERE stable_id LIKE ?;");
    bind_text(delete_symbol_nodes.get(), 1, symbol_stable_prefix);
    delete_symbol_nodes.expect_done("delete old symbol nodes");
}

int64_t insert_symbol_row(sqlite3* db, Statement& insert_symbol_stmt, const FileRow& file, const SymbolInfo& symbol) {
    insert_symbol_stmt.reset();
    bind_int64(insert_symbol_stmt.get(), 1, file.file_id);
    bind_text(insert_symbol_stmt.get(), 2, symbol.kind);
    bind_text(insert_symbol_stmt.get(), 3, symbol.name);
    bind_text(insert_symbol_stmt.get(), 4, symbol.qualified_name);
    bind_text(insert_symbol_stmt.get(), 5, symbol.signature);
    bind_int64(insert_symbol_stmt.get(), 6, symbol.start_line);
    bind_int64(insert_symbol_stmt.get(), 7, symbol.end_line);
    bind_int64(insert_symbol_stmt.get(), 8, symbol.start_byte);
    bind_int64(insert_symbol_stmt.get(), 9, symbol.end_byte);
    bind_text(insert_symbol_stmt.get(), 10, symbol.content_hash);
    bind_text(insert_symbol_stmt.get(), 11, file.commit_hash);
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
        check_sqlite(sqlite3_bind_null(insert_edge_stmt.get(), 2), sqlite3_db_handle(insert_edge_stmt.get()), "bind null edge target");
    }
    bind_text(insert_edge_stmt.get(), 3, to_ref);
    bind_text(insert_edge_stmt.get(), 4, kind);
    bind_int64(insert_edge_stmt.get(), 5, resolved ? 1 : 0);
    insert_edge_stmt.expect_done("insert edge");
}

}  // namespace

IndexResult index_cpp_repository(Storage& storage, const IndexOptions& options) {
    const std::filesystem::path repo_root = std::filesystem::weakly_canonical(options.repo_root);
    const std::vector<FileRow> files = load_cpp_files(storage.handle());

    Statement upsert_node(
        storage.handle(),
        "INSERT INTO nodes(stable_id, kind, title, created_at, status) "
        "VALUES (?, ?, ?, ?, 'active') "
        "ON CONFLICT(stable_id) DO UPDATE SET "
        "kind = excluded.kind, title = excluded.title, status = 'active';"
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
        for (const FileRow& file : files) {
            const std::filesystem::path abs_path = repo_root / file.path;
            const std::string source = read_file_bytes(abs_path);
            const ExtractedFile extracted = extract_cpp_symbols(source);

            const std::string file_stable = file_stable_id(options.repo_id, file.path);
            const int64_t file_node_id = upsert_source_node(
                upsert_node,
                select_node,
                file_stable,
                "file",
                file.path
            );

            const std::string symbol_prefix =
                "symbol:" + options.repo_id + ":" + file.path + "::%";
            delete_file_projection(storage.handle(), file.file_id, file_node_id, symbol_prefix);

            std::vector<SymbolInfo> symbols = extracted.symbols;
            for (SymbolInfo& symbol : symbols) {
                (void)insert_symbol_row(storage.handle(), insert_symbol, file, symbol);
                const std::string stable = symbol_stable_id(options.repo_id, file.path, symbol.qualified_name);
                symbol.node_id = upsert_source_node(
                    upsert_node,
                    select_node,
                    stable,
                    "symbol",
                    symbol.qualified_name
                );
                ++result.symbols_indexed;
            }

            for (size_t i = 0; i < symbols.size(); ++i) {
                const SymbolInfo& symbol = symbols[i];
                const int64_t parent_node =
                    symbol.parent_index >= 0 ? symbols[static_cast<size_t>(symbol.parent_index)].node_id
                                             : file_node_id;
                insert_edge_row(insert_edge, parent_node, symbol.node_id, "", "contains", true);
                ++result.contains_edges;
            }

            for (const IncludeInfo& include : extracted.includes) {
                insert_edge_row(insert_edge, file_node_id, -1, include.target, "imports", false);
                ++result.imports_edges;
            }

            ++result.files_indexed;
        }

        storage.execute("COMMIT;");
    } catch (...) {
        storage.execute("ROLLBACK;");
        throw;
    }

    return result;
}

}  // namespace codegraph
