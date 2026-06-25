#include <cstdlib>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bootstrap.h"
#include "core.h"
#include "cpp_frontend.h"
#include "graph_store.h"
#include "indexer.h"
#include "materializer.h"
#include "memory_reads.h"
#include "mcp_server.h"
#include "read_tools.h"
#include "resolver.h"
#include "scanner.h"
#include "source_projection.h"
#include "sqlite_util.h"
#include "storage.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <tree_sitter/api.h>
#include "xxhash.h"

extern "C" {
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_python();
}




bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

const TSLanguage* language_for_path(std::string_view path) {
    if (ends_with(path, ".cpp") ||
        ends_with(path, ".cc") ||
        ends_with(path, ".cxx") ||
        ends_with(path, ".h") ||
        ends_with(path, ".hpp")) {
        return tree_sitter_cpp();
    }

    if (ends_with(path, ".py")) {
        return tree_sitter_python();
    }

    return nullptr;
}

const char* language_name_for_path(std::string_view path) {
    if (ends_with(path, ".cpp") ||
        ends_with(path, ".cc") ||
        ends_with(path, ".cxx") ||
        ends_with(path, ".h") ||
        ends_with(path, ".hpp")) {
        return "cpp";
    }

    if (ends_with(path, ".py")) {
        return "python";
    }

    return "unknown";
}

namespace {

constexpr const char* kVersion = "0.1.0";

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

int run_command_count_lines(const std::string& command) {
    std::array<char, 256> buffer{};
    int lines = 0;

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run command: " + command);
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        ++lines;
    }

    const int rc = pclose(pipe);
    if (rc != 0 && rc != 256) {
        throw std::runtime_error("command failed: " + command);
    }
    return lines;
}

std::string run_command_capture(const std::string& command) {
    std::array<char, 512> buffer{};
    std::string output;

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run command: " + command);
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int rc = pclose(pipe);
    if (rc != 0) {
        throw std::runtime_error("command failed: " + command + "\n" + output);
    }
    return output;
}

uint32_t rg_text_count(const std::filesystem::path& repo_root, const std::string& text) {
    const std::string command =
        "rg --fixed-strings --line-number --color never " +
        shell_quote(text) + " " + shell_quote(repo_root.string());
    return static_cast<uint32_t>(run_command_count_lines(command));
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    output << contents;
}

struct RemoveOnExit {
    std::filesystem::path path;

    ~RemoveOnExit() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

struct RemoveTreeOnExit {
    std::filesystem::path path;

    ~RemoveTreeOnExit() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

codegraph::FrontendRegistry make_frontend_registry() {
    codegraph::FrontendRegistry registry;
    registry.add(std::make_unique<codegraph::CppFrontend>());
    return registry;
}

std::vector<std::string> collect_repeated_values(
    const std::vector<std::string>& args,
    std::string_view option
) {
    std::vector<std::string> values;
    for (size_t i = 0; i + 1U < args.size(); ++i) {
        if (args[i] == option) {
            values.push_back(args[i + 1U]);
        }
    }
    return values;
}

std::string single_option_value(
    const std::vector<std::string>& args,
    std::string_view option,
    bool required
) {
    for (size_t i = 0; i + 1U < args.size(); ++i) {
        if (args[i] == option) {
            return args[i + 1U];
        }
    }
    if (required) {
        throw std::runtime_error("missing required option: " + std::string(option));
    }
    return {};
}

void sqlite_exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : "unknown sqlite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

int count_callback(void* data, int argc, char** argv, char** col_names) {
    (void)argc;
    (void)argv;
    (void)col_names;
    auto* count = static_cast<int*>(data);
    ++(*count);
    return 0;
}

void check_sqlite_fts5() {
    sqlite3* db = nullptr;

    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error(message);
    }

    try {
        sqlite_exec(db, "CREATE VIRTUAL TABLE fts_test USING fts5(x);");
        sqlite_exec(db, "INSERT INTO fts_test(x) VALUES ('hello codegraph');");

        int rows = 0;
        char* error = nullptr;
        const int rc = sqlite3_exec(
            db,
            "SELECT rowid FROM fts_test WHERE fts_test MATCH 'hello';",
            count_callback,
            &rows,
            &error
        );

        if (rc != SQLITE_OK) {
            std::string message = error ? error : "FTS5 query failed";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }

        if (rows != 1) {
            throw std::runtime_error("FTS5 query returned unexpected row count");
        }

        sqlite3_close(db);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
}

void check_git() {
    const int rc = std::system("git --version > /dev/null 2>&1");
    if (rc != 0) {
        throw std::runtime_error("git is not available on PATH");
    }
}

void check_xxhash() {
    const char* input = "hello codegraph";
    const uint64_t expected = 1981480317691811586; // precomputed value for "hello codegraph" with seed 0
    const uint64_t actual = XXH64(input, std::strlen(input), 0);
    if (actual != expected) {
        throw std::runtime_error("xxhash64 produced unexpected result");
    }
   
}


void parse_source(const std::string& source,
                  const TSLanguage* language,
                  const std::string& language_name,
                  bool print_tree) {
    if (language == nullptr) {
        throw std::runtime_error("unsupported or unavailable tree-sitter language");
    }

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        throw std::runtime_error("failed to allocate tree-sitter parser");
    }

    if (!ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        throw std::runtime_error("failed to set tree-sitter language: " + language_name);
    }

    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        source.c_str(),
        static_cast<uint32_t>(source.size())
    );

    if (tree == nullptr) {
        ts_parser_delete(parser);
        throw std::runtime_error("tree-sitter parse failed");
    }

    TSNode root = ts_tree_root_node(tree);

    std::cout << "tree-sitter-" << language_name << ": ok\n";
    std::cout << "root_type: " << ts_node_type(root) << "\n";
    std::cout << "has_error: " << (ts_node_has_error(root) ? "true" : "false") << "\n";

    if (print_tree) {
        char* sexpr = ts_node_string(root);
        if (sexpr != nullptr) {
            std::cout << sexpr << "\n";
            std::free(sexpr);
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_throw_invalid_string_id(const codegraph::StringInterner& interner) {
    try {
        (void)interner.view(static_cast<codegraph::StringId>(9999U));
    } catch (const std::out_of_range&) {
        return;
    }

    throw std::runtime_error("invalid StringId did not throw");
}

void expect_schema_objects(const codegraph::Storage& storage) {
    const std::vector<std::string_view> tables{
        "files",
        "line_tables",
        "symbols",
        "nodes",
        "edges",
        "memories",
        "path_rules",
        "op_index",
        "fts_symbols",
        "fts_memories",
    };
    for (const std::string_view table : tables) {
        require(storage.object_exists("table", table), "missing sqlite table: " + std::string(table));
    }

    const std::vector<std::string_view> indexes{
        "idx_sym_name",
        "idx_sym_qual",
        "idx_sym_file",
        "idx_nodes_kind",
        "idx_edges_from",
        "idx_edges_to",
        "idx_edges_unresolved",
    };
    for (const std::string_view index : indexes) {
        require(storage.object_exists("index", index), "missing sqlite index: " + std::string(index));
    }

    const std::vector<std::string_view> triggers{
        "symbols_ai",
        "symbols_ad",
        "symbols_au",
        "memories_ai",
        "memories_ad",
        "memories_au",
    };
    for (const std::string_view trigger : triggers) {
        require(storage.object_exists("trigger", trigger), "missing sqlite trigger: " + std::string(trigger));
    }

    require(storage.query_int("PRAGMA user_version;") == 2, "unexpected sqlite user_version");
}


int command_version() {
    std::cout << "codegraph " << kVersion << "\n";
    return 0;
}

std::filesystem::path command_repo_root(const std::string* path) {
    if (path == nullptr) {
        return std::filesystem::current_path();
    }
    return std::filesystem::weakly_canonical(*path);
}

int command_doctor_deps() {
    std::cout << "codegraph dependency doctor\n";

    std::cout << "sqlite: " << sqlite3_libversion() << "\n";
    check_sqlite_fts5();
    std::cout << "sqlite fts5: ok\n";

    check_git();
    std::cout << "git: ok\n";

    nlohmann::json j = {
        {"json", "ok"},
        {"project", "codegraph"}
    };
    if (j.at("json") != "ok") {
        throw std::runtime_error("nlohmann/json smoke test failed");
    }
    std::cout << "nlohmann/json: ok\n";

    check_xxhash();
    std::cout << "xxhash: ok\n";

    parse_source(
    "int add(int a, int b) { return a + b; }\n",
    tree_sitter_cpp(),
    "cpp",
    false
    );

    parse_source(
        "def add(a, b):\n    return a + b\n",
        tree_sitter_python(),
        "python",
        false
    );




    std::cout << "all dependencies: ok\n";
    return 0;
}

struct DoctorCheck {
    std::string name;
    int64_t failures = 0;
};

int command_doctor(const std::string* path) {
    const std::filesystem::path repo_root = command_repo_root(path);
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";
    const std::filesystem::path db_path = codegraph_dir / "graph.sqlite";

    std::cout << "codegraph doctor\n";
    std::cout << "repo: " << repo_root.generic_string() << "\n";
    std::cout << "db: " << db_path.generic_string() << "\n";

    if (!std::filesystem::exists(db_path)) {
        std::cout << "initialized: no\n";
        std::cout << "doctor: failed\n";
        return 1;
    }

    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    std::vector<DoctorCheck> checks{
        {"schema_user_version", storage.query_int("PRAGMA user_version;") == 2 ? 0 : 1},
        {"dup_file_nodes",
         storage.query_int(
             "SELECT COUNT(*) - COUNT(DISTINCT title) FROM nodes WHERE kind = 'file';"
         )},
        {"files_without_line_table",
         storage.query_int(
             "SELECT COUNT(*) FROM files f "
             "LEFT JOIN line_tables lt ON lt.file_id = f.file_id "
             "WHERE lt.file_id IS NULL;"
         )},
        {"line_tables_without_file",
         storage.query_int(
             "SELECT COUNT(*) FROM line_tables lt "
             "LEFT JOIN files f ON f.file_id = lt.file_id "
             "WHERE f.file_id IS NULL;"
         )},
        {"symbols_without_file",
         storage.query_int(
             "SELECT COUNT(*) FROM symbols s "
             "LEFT JOIN files f ON f.file_id = s.file_id "
             "WHERE f.file_id IS NULL;"
         )},
        {"nodes_bad_kind",
         storage.query_int(
             "SELECT COUNT(*) FROM nodes "
             "WHERE kind NOT IN ('file','symbol','correction','arch_decision', 'handoff');"
         )},
        {"nodes_bad_status",
         storage.query_int(
             "SELECT COUNT(*) FROM nodes "
             "WHERE status NOT IN ('active','tombstoned','stale');"
         )},
        {"edges_without_from_node",
         storage.query_int(
             "SELECT COUNT(*) FROM edges e "
             "LEFT JOIN nodes n ON n.node_id = e.from_node "
             "WHERE n.node_id IS NULL;"
         )},
        {"resolved_edges_without_to_node",
         storage.query_int(
             "SELECT COUNT(*) FROM edges e "
             "LEFT JOIN nodes n ON n.node_id = e.to_node "
             "WHERE e.resolved = 1 AND (e.to_node IS NULL OR n.node_id IS NULL);"
         )},
        {"resolved_edges_with_to_ref_only",
         storage.query_int(
             "SELECT COUNT(*) FROM edges "
             "WHERE resolved = 1 AND to_node IS NULL;"
         )},
        {"memories_without_node",
         storage.query_int(
             "SELECT COUNT(*) FROM memories m "
             "LEFT JOIN nodes n ON n.node_id = m.node_id "
             "WHERE n.node_id IS NULL;"
         )},
        {"path_rules_without_node",
         storage.query_int(
             "SELECT COUNT(*) FROM path_rules pr "
             "LEFT JOIN nodes n ON n.node_id = pr.node_id "
             "WHERE n.node_id IS NULL;"
         )},
        {"fts_symbols_row_drift",
         storage.query_int(
             "SELECT ABS((SELECT COUNT(*) FROM symbols) - "
             "(SELECT COUNT(*) FROM fts_symbols));"
         )},
        {"fts_memories_row_drift",
         storage.query_int(
             "SELECT ABS((SELECT COUNT(*) FROM memories) - "
             "(SELECT COUNT(*) FROM fts_memories));"
         )},
    };

    int64_t total_failures = 0;
    for (const DoctorCheck& check : checks) {
        total_failures += check.failures;
        std::cout << check.name << ": " << check.failures << "\n";
    }
    std::cout << "files: " << storage.query_int("SELECT COUNT(*) FROM files;") << "\n";
    std::cout << "symbols: " << storage.query_int("SELECT COUNT(*) FROM symbols;") << "\n";
    std::cout << "nodes: " << storage.query_int("SELECT COUNT(*) FROM nodes;") << "\n";
    std::cout << "edges: " << storage.query_int("SELECT COUNT(*) FROM edges;") << "\n";
    std::cout << "memories: " << storage.query_int("SELECT COUNT(*) FROM memories;") << "\n";

    if (total_failures != 0) {
        std::cout << "doctor: failed\n";
        return 1;
    }

    std::cout << "doctor: ok\n";
    return 0;
}

int command_parse_smoke(const std::string& path) {
    const std::string source = read_file(path);
    const TSLanguage* language = language_for_path(path);
    const std::string language_name = language_name_for_path(path);

    if (language == nullptr) {
        throw std::runtime_error("unsupported file extension: " + path);
    }

    std::cout << "parse-smoke: " << path << "\n";
    parse_source(source, language, language_name, true);
    return 0;
}

int command_test_core() {
    codegraph::StringInterner interner;

    const auto alpha = interner.intern("alpha");
    const auto beta = interner.intern("beta");
    const auto alpha_again = interner.intern("alpha");
    const auto empty = interner.intern("");

    require(alpha == alpha_again, "interner did not deduplicate repeated string");
    require(alpha != beta, "interner returned the same id for distinct strings");
    require(interner.view(alpha) == "alpha", "interner view(alpha) mismatch");
    require(interner.view(beta) == "beta", "interner view(beta) mismatch");
    require(interner.view(empty).empty(), "interner view(empty) mismatch");
    require(interner.size() == 3U, "interner size mismatch after deduplication");

    for (uint32_t i = 0; i < 512U; ++i) {
        const std::string value = "name::" + std::to_string(i);
        const auto id = interner.intern(value);
        require(interner.view(id) == value, "interner view mismatch after growth");
    }

    require(interner.view(alpha) == "alpha", "interner did not preserve old view after growth");
    require(interner.intern("alpha") == alpha, "interner lookup failed after growth");
    expect_throw_invalid_string_id(interner);

    const codegraph::SourceSpan span{
        3U,
        11U,
        42U,
        315U,
        0x0123456789ABCDEFULL,
        0x000BU,
    };
    const auto unpacked = codegraph::unpack_source_span(codegraph::pack_source_span(span));
    require(unpacked.start_line == span.start_line, "span start_line round-trip mismatch");
    require(unpacked.end_line == span.end_line, "span end_line round-trip mismatch");
    require(unpacked.start_byte == span.start_byte, "span start_byte round-trip mismatch");
    require(unpacked.end_byte == span.end_byte, "span end_byte round-trip mismatch");
    require(unpacked.content_hash == span.content_hash, "span content_hash round-trip mismatch");
    require(unpacked.flags == span.flags, "span flags round-trip mismatch");

    require(codegraph::node_kind_text(codegraph::NodeKind::Symbol) == "symbol", "node kind text mismatch");
    require(
        codegraph::node_kind_from_string("arch_decision") == codegraph::NodeKind::ArchDecision,
        "node kind parse mismatch"
    );
    require(
        codegraph::symbol_kind_text(codegraph::SymbolKind::Field) == "field",
        "symbol kind text mismatch"
    );
    require(
        codegraph::symbol_kind_from_string("method") == codegraph::SymbolKind::Method,
        "symbol kind parse mismatch"
    );
    require(
        codegraph::edge_kind_from_string("affects") == codegraph::EdgeKind::Affects,
        "edge kind parse mismatch"
    );
    require(
        codegraph::memory_type_from_string("correction") == codegraph::MemoryType::Correction,
        "memory type parse mismatch"
    );
    codegraph::SymbolStableIdParts parsed;
    require(
        codegraph::parse_symbol_stable_id(
            codegraph::symbol_stable_id("repo", "src/x.cc", "ns::thing"),
            parsed
        ),
        "symbol stable id did not parse"
    );
    require(parsed.repo_id == "repo", "symbol stable repo parse mismatch");
    require(parsed.path == "src/x.cc", "symbol stable path parse mismatch");
    require(parsed.qualified_name == "ns::thing", "symbol stable qualified parse mismatch");

    std::cout << "core types: ok\n";
    std::cout << "string interner: ok\n";
    std::cout << "source span pack/unpack: ok\n";
    std::cout << "kind vocabulary: ok\n";
    std::cout << "stable ids: ok\n";
    return 0;
}

int command_test_storage() {
    const std::filesystem::path db_path =
        std::filesystem::current_path() / "build" / "codegraph-test-storage.sqlite";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);

    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        expect_schema_objects(storage);

        storage.execute(
            "INSERT INTO files(file_id, path, language, content_hash, size_bytes, line_count, indexed_at) "
            "VALUES (1, 'src/example.cpp', 'cpp', 'filehash', 128, 8, '2026-06-18T00:00:00Z');"
        );
        storage.execute(
            "INSERT INTO symbols(symbol_id, file_id, kind, name, qualified_name, signature, "
            "start_line, end_line, start_byte, end_byte, content_hash) "
            "VALUES (1, 1, 'function', 'make_graph', 'cg::make_graph', 'make_graph()', "
            "1, 4, 0, 64, 'spanhash');"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM fts_symbols WHERE fts_symbols MATCH 'make_graph';") == 1,
            "fts_symbols trigger did not index inserted symbol"
        );

        storage.execute(
            "INSERT INTO nodes(node_id, stable_id, kind, title, created_at) "
            "VALUES (1, 'memory:test-device:1', 'correction', 'Prefer storage', '2026-06-18T00:00:00Z');"
        );
        storage.execute(
            "INSERT INTO memories(memory_id, node_id, memory_type, title, body, created_at) "
            "VALUES (1, 1, 'correction', 'Prefer storage', 'Use the schema module for storage.', "
            "'2026-06-18T00:00:00Z');"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM fts_memories WHERE fts_memories MATCH 'schema';") == 1,
            "fts_memories trigger did not index inserted memory"
        );
    }

    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        expect_schema_objects(storage);
        require(
            storage.query_int("SELECT COUNT(*) FROM files WHERE path = 'src/example.cpp';") == 1,
            "reopening schema initialization did not preserve existing rows"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM fts_symbols WHERE fts_symbols MATCH 'make_graph';") == 1,
            "reopened fts_symbols query failed"
        );
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "sqlite schema: ok\n";
    std::cout << "schema reopen: ok\n";
    std::cout << "fts triggers: ok\n";
    return 0;
}

int command_init(const std::string* path) {
    const std::filesystem::path repo_root = command_repo_root(path);
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";
    const std::filesystem::path db_path = codegraph_dir / "graph.sqlite";
    codegraph::FrontendRegistry registry = make_frontend_registry();

    std::filesystem::create_directories(codegraph_dir);
    codegraph::Storage storage(db_path);
    const codegraph::BootstrapResult result =
        codegraph::bootstrap_repository(storage, registry, repo_root, codegraph_dir);

    std::cout << "init: ok\n";
    std::cout << "repo: " << repo_root.generic_string() << "\n";
    std::cout << "repo_id: " << result.config.repo_id << "\n";
    std::cout << "db: " << db_path.generic_string() << "\n";
    std::cout << "device_id: " << codegraph::ensure_device_id(codegraph_dir) << "\n";
    std::cout << "scan_files_seen: " << result.scan.files_seen << "\n";
    std::cout << "scan_files_indexed: " << result.scan.files_indexed << "\n";
    std::cout << "scan_files_unchanged: " << result.scan.files_unchanged << "\n";
    std::cout << "indexed_files: " << result.index.files_indexed << "\n";
    std::cout << "index_files_unchanged: " << result.index.files_unchanged << "\n";
    std::cout << "ops_applied: " << result.materialize.ops_applied << "\n";
    std::cout << "edges_resolved: " << result.materialize.edges_resolved << "\n";
    return 0;
}

int command_scan(const std::string* path) {
    const std::filesystem::path repo_root = command_repo_root(path);
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    const codegraph::RepoConfig config = codegraph::load_or_create_config(repo_root);
    codegraph::FrontendRegistry registry = make_frontend_registry();

    std::filesystem::create_directories(db_path.parent_path());
    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const codegraph::ScanResult result = codegraph::scan_repository(
        storage,
        registry,
        codegraph::scan_options_for_config(repo_root, config)
    );

    std::cout << "scan: ok\n";
    std::cout << "repo: " << repo_root.generic_string() << "\n";
    std::cout << "db: " << db_path.generic_string() << "\n";
    std::cout << "branch: " << result.branch << "\n";
    std::cout << "commit: " << result.commit_hash << "\n";
    std::cout << "files_seen: " << result.files_seen << "\n";
    std::cout << "files_indexed: " << result.files_indexed << "\n";
    std::cout << "files_unchanged: " << result.files_unchanged << "\n";
    std::cout << "files_pruned: " << result.files_pruned << "\n";
    std::cout << "bytes_indexed: " << result.bytes_indexed << "\n";
    return 0;
}

int command_index(const std::string* path) {
    const std::filesystem::path repo_root = command_repo_root(path);
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";
    const std::filesystem::path db_path = codegraph_dir / "graph.sqlite";
    const codegraph::RepoConfig config = codegraph::load_or_create_config(repo_root);
    codegraph::FrontendRegistry registry = make_frontend_registry();

    std::filesystem::create_directories(db_path.parent_path());
    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const codegraph::ScanResult scan_result = codegraph::scan_repository(
        storage,
        registry,
        codegraph::scan_options_for_config(repo_root, config)
    );
    const codegraph::IndexResult index_result = codegraph::index_repository(
        storage,
        registry,
        codegraph::index_options_for_config(repo_root, config)
    );
    const codegraph::MaterializeResult materialized =
        codegraph::materialize(storage, codegraph_dir);

    std::cout << "index: ok\n";
    std::cout << "repo: " << repo_root.generic_string() << "\n";
    std::cout << "db: " << db_path.generic_string() << "\n";
    std::cout << "scan_files_seen: " << scan_result.files_seen << "\n";
    std::cout << "scan_files_indexed: " << scan_result.files_indexed << "\n";
    std::cout << "scan_files_unchanged: " << scan_result.files_unchanged << "\n";
    std::cout << "scan_files_pruned: " << scan_result.files_pruned << "\n";
    std::cout << "indexed_files: " << index_result.files_indexed << "\n";
    std::cout << "index_files_unchanged: " << index_result.files_unchanged << "\n";
    std::cout << "index_files_pruned: " << index_result.files_pruned << "\n";
    std::cout << "symbols_indexed: " << index_result.symbols_indexed << "\n";
    std::cout << "contains_edges: " << index_result.contains_edges << "\n";
    std::cout << "imports_edges: " << index_result.imports_edges << "\n";
    std::cout << "ops_applied: " << materialized.ops_applied << "\n";
    std::cout << "edges_resolved: " << materialized.edges_resolved << "\n";
    return 0;
}

int command_find_symbol(const std::string& query) {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";

    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const std::vector<codegraph::SymbolMatch> matches = codegraph::find_symbols(storage, query);
    for (const codegraph::SymbolMatch& match : matches) {
        std::cout << match.qualified_name
                  << "\t" << match.kind
                  << "\t" << match.path << ":" << match.start_line << "-" << match.end_line
                  << "\t" << "symbol_id=" << match.symbol_id
                  << "\n";
    }
    return 0;
}

int command_search_symbol(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("usage: codegraph search-symbol <query> [--kind K] [--limit N]");
    }

    std::string kind;
    uint32_t limit = 20;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--kind" && i + 1U < args.size()) {
            kind = args[++i];
            continue;
        }
        if (args[i] == "--limit" && i + 1U < args.size()) {
            limit = static_cast<uint32_t>(std::stoul(args[++i]));
            continue;
        }
        throw std::runtime_error("usage: codegraph search-symbol <query> [--kind K] [--limit N]");
    }

    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";

    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const std::vector<codegraph::SymbolSearchMatch> matches =
        kind.empty()
            ? codegraph::search_symbols(storage, args[0], std::nullopt, limit)
            : codegraph::search_symbols(storage, args[0], std::string_view(kind), limit);
    for (const codegraph::SymbolSearchMatch& match : matches) {
        std::cout << match.qualified_name
                  << "\t" << match.kind
                  << "\t" << match.path << ":" << match.start_line << "-" << match.end_line
                  << "\t" << "symbol_id=" << match.symbol_id
                  << "\t" << "score=" << match.score
                  << "\t" << "signature=" << match.signature
                  << "\n";
    }
    return 0;
}

int command_read_symbol(const std::string& query) {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    codegraph::FrontendRegistry registry = make_frontend_registry();

    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const codegraph::ReadSymbolResult result = codegraph::read_symbol_verified(
        storage,
        registry,
        codegraph::IndexOptions{repo_root},
        query
    );

    std::cout << "status: " << codegraph::read_status_name(result.status) << "\n";
    if (!result.message.empty()) {
        std::cout << "message: " << result.message << "\n";
    }
    if (result.status == codegraph::ReadStatus::NotFound) {
        return 1;
    }
    if (result.status == codegraph::ReadStatus::Gone) {
        std::cout << "qualified_name: " << result.symbol.qualified_name << "\n";
        std::cout << "path: " << result.symbol.path << "\n";
        return 0;
    }

    std::cout << "qualified_name: " << result.symbol.qualified_name << "\n";
    std::cout << "kind: " << result.symbol.kind << "\n";
    std::cout << "path: " << result.symbol.path << "\n";
    std::cout << "lines: " << result.symbol.start_line << "-" << result.symbol.end_line << "\n";
    std::cout << "bytes: " << result.symbol.start_byte << "-" << result.symbol.end_byte << "\n";
    std::cout << "body:\n" << result.body << "\n";
    return 0;
}

int command_read_file(const std::vector<std::string>& args) {
    if (args.size() != 5U || args[1] != "--start" || args[3] != "--end") {
        throw std::runtime_error("usage: codegraph read-file <path> --start N --end M");
    }

    const std::filesystem::path repo_root = std::filesystem::current_path();
    const uint32_t start_line = static_cast<uint32_t>(std::stoul(args[2]));
    const uint32_t end_line = static_cast<uint32_t>(std::stoul(args[4]));
    const codegraph::FileRangeResult result =
        codegraph::read_file_range(repo_root, args[0], start_line, end_line);

    std::cout << "path: " << result.path << "\n";
    std::cout << "lines: " << result.start_line << "-" << result.end_line << "\n";
    std::cout << "current_hash: " << result.content_hash << "\n";
    std::cout << "text:\n" << result.text;
    return 0;
}

int command_materialize() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";

    std::filesystem::create_directories(codegraph_dir);
    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    const codegraph::MaterializeResult result =
        codegraph::materialize(storage, codegraph_dir);
    std::cout << "materialize: ok\n";
    std::cout << "ops_applied: " << result.ops_applied << "\n";
    std::cout << "edges_resolved: " << result.edges_resolved << "\n";
    return 0;
}

int command_remember(const std::vector<std::string>& args) {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";

    codegraph::DecisionInput input{
        single_option_value(args, "--title", true),
        single_option_value(args, "--body", true),
        collect_repeated_values(args, "--affects"),
    };
    if (input.affects.empty()) {
        throw std::runtime_error("remember requires at least one --affects value");
    }

    std::filesystem::create_directories(codegraph_dir);
    const std::string op_id = codegraph::append_decision_op(codegraph_dir, input);

    codegraph::Storage storage(db_path);
    storage.initialize_schema();
    const codegraph::MaterializeResult result = codegraph::materialize(storage, codegraph_dir);

    std::cout << "remember: ok\n";
    std::cout << "op_id: " << op_id << "\n";
    std::cout << "ops_applied: " << result.ops_applied << "\n";
    std::cout << "edges_resolved: " << result.edges_resolved << "\n";
    return 0;
}

int command_correct(const std::vector<std::string>& args) {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";

    codegraph::CorrectionInput input;
    input.title = single_option_value(args, "--title", false);
    input.reason = single_option_value(args, "--reason", true);
    input.prefer_paths = collect_repeated_values(args, "--prefer");
    input.avoid_paths = collect_repeated_values(args, "--avoid");
    input.affects = collect_repeated_values(args, "--affects");
    if (input.title.empty()) {
        input.title = input.reason;
    }
    if (input.affects.empty()) {
        throw std::runtime_error("correct requires at least one --affects value");
    }
    if (input.prefer_paths.empty() && input.avoid_paths.empty()) {
        throw std::runtime_error("correct requires at least one --prefer or --avoid value");
    }

    std::filesystem::create_directories(codegraph_dir);
    const std::string op_id = codegraph::append_correction_op(codegraph_dir, input);

    codegraph::Storage storage(db_path);
    storage.initialize_schema();
    const codegraph::MaterializeResult result = codegraph::materialize(storage, codegraph_dir);

    std::cout << "correct: ok\n";
    std::cout << "op_id: " << op_id << "\n";
    std::cout << "ops_applied: " << result.ops_applied << "\n";
    std::cout << "edges_resolved: " << result.edges_resolved << "\n";
    return 0;
}

void print_memory_view(const codegraph::MemoryView& memory) {
    std::cout << "- " << memory.title << "\n";
    std::cout << "  type: " << memory.memory_type << "\n";
    std::cout << "  body: " << memory.body << "\n";
    std::cout << "  provenance: " << memory.provenance << "\n";
    for (const codegraph::PathRuleView& rule : memory.path_rules) {
        std::cout << "  rule: " << rule.rule_kind << " " << rule.pattern << "\n";
        if (!rule.reason.empty()) {
            std::cout << "  reason: " << rule.reason << "\n";
        }
    }
}

int command_memory_for(const std::string& target) {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";

    codegraph::Storage storage(db_path);
    storage.initialize_schema();
    const codegraph::MemoryReadResult result =
        codegraph::memory_for_target(storage, target);

    std::cout << "target: " << result.target << "\n";
    std::cout << "corrections: " << result.corrections.size() << "\n";
    for (const codegraph::MemoryView& memory : result.corrections) {
        print_memory_view(memory);
    }
    std::cout << "decisions: " << result.decisions.size() << "\n";
    for (const codegraph::MemoryView& memory : result.decisions) {
        print_memory_view(memory);
    }
    return 0;
}

uint32_t sql_direct_memory_count(codegraph::Storage& storage, int64_t target_node) {
    codegraph::Statement stmt(
        storage.handle(),
        "SELECT COUNT(DISTINCT m.memory_id) "
        "FROM edges e "
        "JOIN memories m ON m.node_id = e.from_node "
        "JOIN nodes n ON n.node_id = m.node_id "
        "WHERE e.kind = ? AND e.resolved = 1 AND e.to_node = ? AND n.status = ?;"
    );
    codegraph::bind_text(stmt.get(), 1, codegraph::edge_kind_text(codegraph::EdgeKind::Affects));
    codegraph::bind_int64(stmt.get(), 2, target_node);
    codegraph::bind_text(stmt.get(), 3, codegraph::status_text(codegraph::Status::Active));
    stmt.expect_row("count direct memory edges");
    return static_cast<uint32_t>(sqlite3_column_int64(stmt.get(), 0));
}

uint32_t sql_symbol_lookup_count(codegraph::Storage& storage, std::string_view symbol) {
    codegraph::Statement stmt(
        storage.handle(),
        "SELECT COUNT(*) FROM symbols WHERE name = ? OR qualified_name = ?;"
    );
    codegraph::bind_text(stmt.get(), 1, symbol);
    codegraph::bind_text(stmt.get(), 2, symbol);
    stmt.expect_row("count symbol lookup");
    return static_cast<uint32_t>(sqlite3_column_int64(stmt.get(), 0));
}

uint64_t elapsed_ns(const std::chrono::steady_clock::time_point& start,
                    const std::chrono::steady_clock::time_point& end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
    );
}

int command_bench(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("usage: codegraph bench lookup|memory-for|read|index <target> [repetitions]");
    }

    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path = repo_root / ".codegraph" / "graph.sqlite";
    codegraph::Storage storage(db_path);
    storage.initialize_schema();

    if (args[0] == "index") {
        const uint32_t repetitions = args.size() >= 2U ? static_cast<uint32_t>(std::stoul(args[1])) : 1U;
        codegraph::FrontendRegistry registry = make_frontend_registry();
        const codegraph::RepoConfig config = codegraph::load_or_create_config(repo_root);

        uint32_t scan_indexed = 0;
        uint32_t index_indexed = 0;
        uint32_t index_unchanged = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            const codegraph::ScanResult scan = codegraph::scan_repository(
                storage,
                registry,
                codegraph::scan_options_for_config(repo_root, config)
            );
            const codegraph::IndexResult index = codegraph::index_repository(
                storage,
                registry,
                codegraph::index_options_for_config(repo_root, config)
            );
            scan_indexed += scan.files_indexed;
            index_indexed += index.files_indexed;
            index_unchanged += index.files_unchanged;
        }
        const auto end = std::chrono::steady_clock::now();

        std::cout << "bench: index\n";
        std::cout << "repetitions: " << repetitions << "\n";
        std::cout << "scan_files_indexed: " << scan_indexed << "\n";
        std::cout << "indexed_files: " << index_indexed << "\n";
        std::cout << "index_files_unchanged: " << index_unchanged << "\n";
        std::cout << "ns_total: " << elapsed_ns(start, end) << "\n";
        return 0;
    }

    if (args[0] == "lookup") {
        if (args.size() < 2U) {
            throw std::runtime_error("usage: codegraph bench lookup <symbol> [repetitions]");
        }
        const uint32_t repetitions = args.size() >= 3U ? static_cast<uint32_t>(std::stoul(args[2])) : 10U;
        const codegraph::GraphIndex graph = codegraph::build_graph_index(storage);

        size_t graph_count = 0;
        const auto graph_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            graph_count += codegraph::graph_symbols_by_name_hash(graph, args[1]).size();
        }
        const auto graph_end = std::chrono::steady_clock::now();

        uint32_t sql_count = 0;
        const auto sql_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            sql_count += sql_symbol_lookup_count(storage, args[1]);
        }
        const auto sql_end = std::chrono::steady_clock::now();

        uint32_t rg_count = 0;
        const auto rg_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            rg_count += rg_text_count(repo_root, args[1]);
        }
        const auto rg_end = std::chrono::steady_clock::now();

        std::cout << "bench: lookup\n";
        std::cout << "repetitions: " << repetitions << "\n";
        std::cout << "graph_symbol_count: " << graph_count << "\n";
        std::cout << "sql_symbol_count: " << sql_count << "\n";
        std::cout << "rg_text_count: " << rg_count << "\n";
        std::cout << "graph_ns_total: " << elapsed_ns(graph_start, graph_end) << "\n";
        std::cout << "sql_ns_total: " << elapsed_ns(sql_start, sql_end) << "\n";
        std::cout << "rg_ns_total: " << elapsed_ns(rg_start, rg_end) << "\n";
        std::cout << "rg_note: text search is inexact and may include comments, calls, and strings\n";
        return 0;
    }

    if (args[0] == "memory-for") {
        if (args.size() < 2U) {
            throw std::runtime_error("usage: codegraph bench memory-for <target> [repetitions]");
        }
        const uint32_t repetitions = args.size() >= 3U ? static_cast<uint32_t>(std::stoul(args[2])) : 10U;
        const int64_t target_node = codegraph::resolve_reference(storage, args[1]);
        if (target_node < 0) {
            throw std::runtime_error("bench target does not resolve to a source node");
        }

        const codegraph::GraphIndex graph = codegraph::build_graph_index(storage);
        const auto node = static_cast<codegraph::NodeId>(static_cast<uint32_t>(target_node));

        uint32_t sql_count = 0;
        const auto sql_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            sql_count += sql_direct_memory_count(storage, target_node);
        }
        const auto sql_end = std::chrono::steady_clock::now();

        uint32_t csr_count = 0;
        const auto csr_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            csr_count += codegraph::graph_memory_count_for_node(graph, node);
        }
        const auto csr_end = std::chrono::steady_clock::now();

        std::cout << "bench: memory-for\n";
        std::cout << "target: " << args[1] << "\n";
        std::cout << "repetitions: " << repetitions << "\n";
        std::cout << "sql_count: " << sql_count << "\n";
        std::cout << "csr_count: " << csr_count << "\n";
        std::cout << "sql_ns_total: " << elapsed_ns(sql_start, sql_end) << "\n";
        std::cout << "csr_ns_total: " << elapsed_ns(csr_start, csr_end) << "\n";
        return 0;
    }

    if (args[0] == "read") {
        if (args.size() < 2U) {
            throw std::runtime_error("usage: codegraph bench read <symbol> [repetitions]");
        }
        const uint32_t repetitions = args.size() >= 3U ? static_cast<uint32_t>(std::stoul(args[2])) : 10U;
        codegraph::FrontendRegistry registry = make_frontend_registry();
        size_t bytes = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < repetitions; ++i) {
            const codegraph::ReadSymbolResult result = codegraph::read_symbol_verified(
                storage,
                registry,
                codegraph::IndexOptions{repo_root},
                args[1]
            );
            bytes += result.body.size();
        }
        const auto end = std::chrono::steady_clock::now();

        std::cout << "bench: read\n";
        std::cout << "repetitions: " << repetitions << "\n";
        std::cout << "bytes_read: " << bytes << "\n";
        std::cout << "ns_total: " << elapsed_ns(start, end) << "\n";
        return 0;
    }

    throw std::runtime_error("unknown bench kind: " + args[0]);
}

int command_test_scan() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-scan.sqlite";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);

    codegraph::ScanResult first_result;
    codegraph::ScanResult second_result;
    codegraph::FrontendRegistry registry = make_frontend_registry();
    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();

        first_result = codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        require(first_result.files_seen > 0, "scanner did not see any C++ files");
        require(first_result.files_indexed > 0, "scanner did not index any C++ files");
        require(!first_result.branch.empty(), "scanner did not capture git branch");
        require(first_result.commit_hash.size() == 40U, "scanner did not capture git HEAD hash");

        require(
            storage.query_int("SELECT COUNT(*) FROM files WHERE path = 'testing/sample.cpp';") == 1,
            "scanner did not store testing/sample.cpp"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM files WHERE path = 'testing/sample.py';") == 0,
            "scanner stored Python file during C++ scanner milestone"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM line_tables WHERE file_id = "
                              "(SELECT file_id FROM files WHERE path = 'testing/sample.cpp');") == 1,
            "scanner did not store line table for testing/sample.cpp"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols;") == 0,
            "scanner populated symbols before tree-sitter extraction milestone"
        );

        const std::vector<uint8_t> stored_line_table = storage.query_blob(
            "SELECT offsets_blob FROM line_tables WHERE file_id = "
            "(SELECT file_id FROM files WHERE path = 'testing/sample.cpp');"
        );
        const std::string sample = read_file("testing/sample.cpp");
        const std::vector<uint32_t> expected_offsets = codegraph::build_line_offsets(sample);
        const std::vector<uint32_t> actual_offsets = codegraph::unpack_line_offsets(stored_line_table);
        require(actual_offsets == expected_offsets, "line table did not round-trip for testing/sample.cpp");

        const size_t namespace_offset = sample.find("namespace demo");
        require(namespace_offset != std::string::npos, "test fixture is missing namespace demo");
        require(actual_offsets.size() >= 3U, "line table has too few lines");
        require(actual_offsets[2] == namespace_offset, "line 3 offset mismatch for testing/sample.cpp");

        second_result = codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        require(second_result.files_unchanged == first_result.files_seen, "second scan did not detect unchanged files");
        require(second_result.files_indexed == 0, "second scan rewrote unchanged files");
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "scan files: ok\n";
    std::cout << "line tables: ok\n";
    std::cout << "scan unchanged: ok\n";
    std::cout << "branch: " << first_result.branch << "\n";
    std::cout << "commit: " << first_result.commit_hash << "\n";
    std::cout << "files_seen: " << first_result.files_seen << "\n";
    std::cout << "files_indexed: " << first_result.files_indexed << "\n";
    std::cout << "files_unchanged_second_scan: " << second_result.files_unchanged << "\n";
    std::cout << "files_pruned_second_scan: " << second_result.files_pruned << "\n";
    return 0;
}

int command_test_index() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-index.sqlite";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);

    const std::filesystem::path anonymous_file = repo_root / "testing" / "codegraph_anonymous_tmp.cpp";
    RemoveOnExit remove_anonymous_file{anonymous_file};
    write_file(
        anonymous_file,
        "namespace {\nint anonymous_index_helper() {\n    return 11;\n}\n}\n"
    );

    codegraph::IndexResult index_result;
    codegraph::FrontendRegistry registry = make_frontend_registry();
    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        index_result = codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

        require(index_result.files_indexed > 0, "indexer did not index any C++ files");
        require(index_result.symbols_indexed > 0, "indexer did not extract any symbols");

        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'demo';") == 1,
            "indexer did not extract namespace demo"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'demo::Greeter';") == 1,
            "indexer did not extract class demo::Greeter"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'demo::Greeter::hello';") == 1,
            "indexer did not extract method demo::Greeter::hello"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'demo::add';") == 1,
            "indexer did not extract function demo::add"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'main' "
                              "AND file_id = (SELECT file_id FROM files WHERE path = 'testing/sample.cpp');") == 1,
            "indexer did not extract function main"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'anonymous_index_helper' "
                              "AND file_id = (SELECT file_id FROM files "
                              "WHERE path = 'testing/codegraph_anonymous_tmp.cpp');") == 1,
            "indexer did not extract function inside anonymous namespace"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE kind = 'method' "
                              "AND qualified_name = 'demo::Greeter::hello';") == 1,
            "indexer did not classify class function as method"
        );

        const int64_t start_byte = storage.query_int(
            "SELECT start_byte FROM symbols WHERE qualified_name = 'demo::add' "
            "AND file_id = (SELECT file_id FROM files WHERE path = 'testing/sample.cpp');"
        );
        const int64_t end_byte = storage.query_int(
            "SELECT end_byte FROM symbols WHERE qualified_name = 'demo::add' "
            "AND file_id = (SELECT file_id FROM files WHERE path = 'testing/sample.cpp');"
        );
        const std::string sample = read_file("testing/sample.cpp");
        const std::string add_body = sample.substr(
            static_cast<size_t>(start_byte),
            static_cast<size_t>(end_byte - start_byte)
        );
        require(
            add_body == "int add(int a, int b) {\n    return a + b;\n}",
            "indexer stored an inexact span for demo::add"
        );

        require(
            storage.query_int("SELECT COUNT(*) FROM fts_symbols WHERE fts_symbols MATCH 'Greeter';") >= 1,
            "fts_symbols was not populated by indexed symbols"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'contains';") >=
                storage.query_int("SELECT COUNT(*) FROM symbols;"),
            "indexer did not create enough Contains edges"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'imports' AND to_ref = 'iostream';") >= 1,
            "indexer did not create Imports edge for #include <iostream>"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind NOT IN ('contains', 'imports');") == 0,
            "indexer created an edge kind outside Step 4 scope"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE file_id IN "
                              "(SELECT file_id FROM files WHERE path = 'testing/sample.py');") == 0,
            "indexer extracted Python symbols during C++ milestone"
        );

        const std::filesystem::path prune_file = repo_root / "testing" / "codegraph_prune_tmp.cpp";
        RemoveOnExit remove_prune_file{prune_file};
        write_file(
            prune_file,
            "namespace prune {\nint prunexyzzy() {\n    return 7;\n}\n}\n"
        );

        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});
        require(
            storage.query_int("SELECT COUNT(*) FROM files WHERE path = 'testing/codegraph_prune_tmp.cpp';") == 1,
            "prune regression fixture was not scanned"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'prune::prunexyzzy';") == 1,
            "prune regression fixture was not indexed"
        );

        std::filesystem::remove(prune_file, ignored);
        const codegraph::ScanResult prune_result =
            codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        require(prune_result.files_pruned >= 1, "scanner did not prune deleted file rows");
        require(
            storage.query_int("SELECT COUNT(*) FROM files WHERE path = 'testing/codegraph_prune_tmp.cpp';") == 0,
            "scanner left stale file row after deletion"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM symbols WHERE qualified_name = 'prune::prunexyzzy';") == 0,
            "scanner left stale symbols after deletion"
        );
        // fts_symbols now indexes symbol bodies, so a fixture-named token can also
        // appear in unrelated source bodies (including this test's own). The real
        // invariant after a delete is that the FTS index has no orphaned rows, i.e.
        // it stays row-for-row in sync with the symbols table.
        require(
            storage.query_int(
                "SELECT ABS((SELECT COUNT(*) FROM symbols) - (SELECT COUNT(*) FROM fts_symbols));"
            ) == 0,
            "scanner left stale FTS symbols after deletion"
        );
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "index symbols: ok\n";
    std::cout << "index spans: ok\n";
    std::cout << "index fts: ok\n";
    std::cout << "index edges: ok\n";
    std::cout << "delete prune: ok\n";
    std::cout << "indexed_files: " << index_result.files_indexed << "\n";
    std::cout << "index_files_pruned: " << index_result.files_pruned << "\n";
    std::cout << "symbols_indexed: " << index_result.symbols_indexed << "\n";
    std::cout << "contains_edges: " << index_result.contains_edges << "\n";
    std::cout << "imports_edges: " << index_result.imports_edges << "\n";
    return 0;
}

int command_test_read() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-read.sqlite";
    const std::filesystem::path read_file_path = repo_root / "testing" / "codegraph_read_tmp.cpp";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);
    RemoveOnExit remove_read_file{read_file_path};

    codegraph::FrontendRegistry registry = make_frontend_registry();
    write_file(
        read_file_path,
        "namespace readstep {\n"
        "int target() {\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int stay() {\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int authCheck() {\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int signatureCarrier(int authCheck) {\n"
        "    return authCheck;\n"
        "}\n"
        "}\n"
    );

    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

        const std::vector<codegraph::SymbolMatch> found =
            codegraph::find_symbols(storage, "readstep::target");
        require(found.size() == 1U, "find-symbol did not find readstep::target");

        const std::vector<codegraph::SymbolSearchMatch> search_found =
            codegraph::search_symbols(storage, "authCheck:* -(", codegraph::KindText::Function, 10);
        require(search_found.size() >= 2U, "search-symbol did not return ranked FTS candidates");
        require(
            search_found.front().qualified_name == "readstep::authCheck",
            "search-symbol did not rank name hit before signature hit"
        );
        require(search_found.front().signature == "authCheck()", "search-symbol did not return signature");
        require(search_found.front().path == "testing/codegraph_read_tmp.cpp", "search-symbol path mismatch");
        require(search_found.front().start_line > 0U, "search-symbol did not return location");
        const std::vector<codegraph::SymbolSearchMatch> limited_search =
            codegraph::search_symbols(storage, "readstep::authCheck", codegraph::KindText::Function, 1);
        require(limited_search.size() == 1U, "search-symbol limit was not applied");

        const codegraph::ReadSymbolResult initial = codegraph::read_symbol_verified(
            storage,
            registry,
            codegraph::IndexOptions{repo_root},
            "readstep::target"
        );
        require(initial.status == codegraph::ReadStatus::Ok, "initial read-symbol was not ok");
        require(
            initial.body == "int target() {\n    return 1;\n}",
            "initial read-symbol returned the wrong body"
        );

        const codegraph::FileRangeResult range =
            codegraph::read_file_range(repo_root, "testing/codegraph_read_tmp.cpp", 1, 2);
        require(
            range.text == "namespace readstep {\nint target() {\n",
            "read-file returned the wrong exact range"
        );

        write_file(
            read_file_path,
            "namespace readstep {\n"
            "int stay() {\n"
            "    return 0;\n"
            "}\n"
            "\n"
            "int target() {\n"
            "    return 2;\n"
            "}\n"
            "}\n"
        );

        const codegraph::ReadSymbolResult moved = codegraph::read_symbol_verified(
            storage,
            registry,
            codegraph::IndexOptions{repo_root},
            "readstep::target"
        );
        require(moved.status == codegraph::ReadStatus::ReResolved, "moved symbol was not re-resolved");
        require(
            moved.body == "int target() {\n    return 2;\n}",
            "re-resolved read-symbol returned the wrong body"
        );
        require(moved.symbol.start_line == 6U, "re-resolved symbol did not move to the expected line");

        write_file(
            read_file_path,
            "namespace readstep {\n"
            "int stay() {\n"
            "    return 0;\n"
            "}\n"
            "}\n"
        );

        const codegraph::ReadSymbolResult deleted = codegraph::read_symbol_verified(
            storage,
            registry,
            codegraph::IndexOptions{repo_root},
            "readstep::target"
        );
        require(deleted.status == codegraph::ReadStatus::Gone, "deleted symbol did not report gone");
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "find symbol: ok\n";
    std::cout << "search symbol: ok\n";
    std::cout << "read symbol: ok\n";
    std::cout << "read file range: ok\n";
    std::cout << "verify before trust: ok\n";
    std::cout << "deleted symbol: ok\n";
    return 0;
}

int command_test_materialize() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-materialize.sqlite";
    const std::filesystem::path codegraph_dir =
        repo_root / "build" / "codegraph-test-materialize-cg";
    const std::filesystem::path pending_file =
        repo_root / "testing" / "codegraph_materialize_pending.cpp";
    const std::string pending_ref = "testing/codegraph_materialize_pending.cpp";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);
    std::filesystem::remove_all(codegraph_dir, ignored);
    std::filesystem::remove(pending_file, ignored);
    RemoveTreeOnExit remove_codegraph_dir{codegraph_dir};
    RemoveOnExit remove_pending_file{pending_file};

    codegraph::FrontendRegistry registry = make_frontend_registry();
    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

        const std::string device_id = codegraph::ensure_device_id(codegraph_dir);
        require(!device_id.empty(), "materializer did not create device_id");

        const std::string correction_op = codegraph::append_correction_op(
            codegraph_dir,
            codegraph::CorrectionInput{
                "Prefer generated fixture",
                "Use the pending fixture once it exists.",
                {"testing/**"},
                {"missing/**"},
                {pending_ref},
            }
        );
        require(!correction_op.empty(), "append correction did not return op_id");

        codegraph::MaterializeResult first = codegraph::materialize(storage, codegraph_dir);
        require(first.ops_applied == 1U, "first materialize did not apply one correction op");
        require(
            storage.query_int("SELECT COUNT(*) FROM memories WHERE memory_type = 'correction';") == 1,
            "correction memory was not materialized"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM path_rules WHERE rule_kind = 'prefer';") == 1,
            "prefer path rule was not materialized"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM path_rules WHERE rule_kind = 'avoid';") == 1,
            "avoid path rule was not materialized"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'affects' "
                              "AND to_ref = 'testing/codegraph_materialize_pending.cpp' "
                              "AND resolved = 0;") == 1,
            "pending correction edge was not left unresolved"
        );

        const std::string decision_op = codegraph::append_decision_op(
            codegraph_dir,
            codegraph::DecisionInput{
                "Keep sample indexed",
                "The sample file anchors materializer tests.",
                {"testing/sample.cpp"},
            }
        );
        require(!decision_op.empty(), "append decision did not return op_id");

        codegraph::MaterializeResult second = codegraph::materialize(storage, codegraph_dir);
        require(second.ops_applied == 1U, "second materialize did not apply one decision op");
        require(
            storage.query_int("SELECT COUNT(*) FROM memories WHERE memory_type = 'arch_decision';") == 1,
            "decision memory was not materialized"
        );
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'affects' "
                              "AND to_ref = 'testing/sample.cpp' AND resolved = 1;") == 1,
            "decision affects edge did not resolve to existing file"
        );

        const int64_t memories_before = storage.query_int("SELECT COUNT(*) FROM memories;");
        const int64_t edges_before = storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'affects';");
        codegraph::MaterializeResult third = codegraph::materialize(storage, codegraph_dir);
        require(third.ops_applied == 0U, "idempotent materialize re-applied ops");
        require(storage.query_int("SELECT COUNT(*) FROM memories;") == memories_before,
                "idempotent materialize duplicated memories");
        require(storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'affects';") == edges_before,
                "idempotent materialize duplicated affects edges");

        write_file(
            pending_file,
            "namespace materialize_pending {\n"
            "int ready() {\n"
            "    return 1;\n"
            "}\n"
            "}\n"
        );
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        require(
            storage.query_int("SELECT COUNT(*) FROM edges WHERE kind = 'affects' "
                              "AND to_ref = 'testing/codegraph_materialize_pending.cpp' "
                              "AND resolved = 1;") == 1,
            "scan did not resolve pending correction edge after file appeared"
        );
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "append ops: ok\n";
    std::cout << "materialize memories: ok\n";
    std::cout << "materialize idempotent: ok\n";
    std::cout << "pending edge resolves after scan: ok\n";
    return 0;
}

int command_test_memory_reads() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-memory.sqlite";
    const std::filesystem::path codegraph_dir =
        repo_root / "build" / "codegraph-test-memory-cg";
    const std::filesystem::path fixture_dir =
        repo_root / "testing" / "codegraph_mem";
    const std::filesystem::path resdb_file =
        fixture_dir / "resdb" / "app" / "x.cc";
    const std::string resdb_ref = "testing/codegraph_mem/resdb/app/x.cc";
    const std::string bftsmart_ref = "testing/codegraph_mem/bftsmart/y.h";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);
    std::filesystem::remove_all(codegraph_dir, ignored);
    std::filesystem::remove_all(fixture_dir, ignored);
    RemoveTreeOnExit remove_codegraph_dir{codegraph_dir};
    RemoveTreeOnExit remove_fixture_dir{fixture_dir};

    std::filesystem::create_directories(resdb_file.parent_path());
    write_file(
        resdb_file,
        "namespace memory_fixture {\n"
        "int anchor() {\n"
        "    return 1;\n"
        "}\n"
        "}\n"
    );

    codegraph::FrontendRegistry registry = make_frontend_registry();
    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

        (void)codegraph::append_correction_op(
            codegraph_dir,
            codegraph::CorrectionInput{
                "Prefer ResDB fixture",
                "Use the ResDB fixture path.",
                {"testing/codegraph_mem/resdb/**"},
                {"testing/codegraph_mem/bftsmart/**"},
                {resdb_ref},
            }
        );
        (void)codegraph::append_decision_op(
            codegraph_dir,
            codegraph::DecisionInput{
                "Anchor memory fixture",
                "The ResDB fixture anchors memory read tests.",
                {resdb_ref},
            }
        );
        const codegraph::MaterializeResult materialized =
            codegraph::materialize(storage, codegraph_dir);
        require(materialized.ops_applied == 2U, "memory read setup did not materialize two ops");

        const codegraph::MemoryReadResult resdb_memory =
            codegraph::memory_for_target(storage, resdb_ref);
        require(resdb_memory.corrections.size() == 1U,
                "memory-for resdb fixture did not return one correction");
        require(resdb_memory.decisions.size() == 1U,
                "memory-for resdb fixture did not return one decision");
        require(
            resdb_memory.corrections[0].body == "Use the ResDB fixture path.",
            "memory-for resdb fixture did not include correction reason"
        );
        bool saw_prefer = false;
        for (const codegraph::PathRuleView& rule : resdb_memory.corrections[0].path_rules) {
            saw_prefer = saw_prefer || rule.rule_kind == "prefer";
        }
        require(saw_prefer, "memory-for resdb fixture did not show prefer rule");

        const codegraph::MemoryReadResult bftsmart_memory =
            codegraph::memory_for_target(storage, bftsmart_ref);
        require(bftsmart_memory.corrections.size() == 1U,
                "memory-for bftsmart fixture did not return avoid correction");
        bool saw_avoid = false;
        for (const codegraph::PathRuleView& rule : bftsmart_memory.corrections[0].path_rules) {
            saw_avoid = saw_avoid || rule.rule_kind == "avoid";
        }
        require(saw_avoid, "memory-for bftsmart fixture did not show avoid rule");

        const codegraph::MemoryReadResult symbol_memory =
            codegraph::memory_for_target(storage, resdb_ref + "::memory_fixture::anchor");
        require(symbol_memory.corrections.size() == 1U,
                "memory-for symbol did not include file path rule correction");
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "memory-for file: ok\n";
    std::cout << "memory-for path rules: ok\n";
    std::cout << "memory-for symbol: ok\n";
    return 0;
}

int command_test_graph() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-graph.sqlite";
    const std::filesystem::path codegraph_dir =
        repo_root / "build" / "codegraph-test-graph-cg";
    const std::filesystem::path fixture_file =
        repo_root / "testing" / "codegraph_graph_target.cpp";
    const std::string target_ref = "testing/codegraph_graph_target.cpp";
    const std::string symbol_name = "graph_target::anchor";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);
    std::filesystem::remove_all(codegraph_dir, ignored);
    std::filesystem::remove(fixture_file, ignored);
    RemoveTreeOnExit remove_codegraph_dir{codegraph_dir};
    RemoveOnExit remove_fixture_file{fixture_file};

    write_file(
        fixture_file,
        "namespace graph_target {\n"
        "int anchor() {\n"
        "    return 8;\n"
        "}\n"
        "}\n"
    );

    codegraph::FrontendRegistry registry = make_frontend_registry();
    {
        codegraph::Storage storage(db_path);
        storage.initialize_schema();
        (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
        (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

        constexpr uint32_t kMemoryCount = 96;
        for (uint32_t i = 0; i < kMemoryCount; ++i) {
            (void)codegraph::append_decision_op(
                codegraph_dir,
                codegraph::DecisionInput{
                    "Graph bench decision " + std::to_string(i),
                    "Decision body " + std::to_string(i),
                    {target_ref},
                }
            );
        }
        const codegraph::MaterializeResult materialized =
            codegraph::materialize(storage, codegraph_dir);
        require(materialized.ops_applied == kMemoryCount,
                "graph test did not materialize expected memories");
        storage.execute(
            "UPDATE nodes SET status = 'tombstoned' "
            "WHERE node_id = (SELECT node_id FROM memories ORDER BY memory_id LIMIT 1);"
        );
        constexpr uint32_t kActiveMemoryCount = kMemoryCount - 1U;

        const int64_t target_node = codegraph::resolve_reference(storage, target_ref);
        require(target_node >= 0, "graph test target did not resolve");

        const codegraph::GraphIndex graph = codegraph::build_graph_index(storage);
        const auto node = static_cast<codegraph::NodeId>(static_cast<uint32_t>(target_node));

        constexpr uint32_t kRepetitions = 10;
        uint32_t sql_total = 0;
        const auto sql_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kRepetitions; ++i) {
            sql_total += sql_direct_memory_count(storage, target_node);
        }
        const auto sql_end = std::chrono::steady_clock::now();

        uint32_t csr_total = 0;
        const auto csr_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kRepetitions; ++i) {
            csr_total += codegraph::graph_memory_count_for_node(graph, node);
        }
        const auto csr_end = std::chrono::steady_clock::now();

        require(sql_total == kActiveMemoryCount * kRepetitions,
                "SQL memory count did not match expected count");
        require(csr_total == sql_total,
                "CSR memory count did not match SQL memory count");

        const uint64_t sql_ns = elapsed_ns(sql_start, sql_end);
        const uint64_t csr_ns = elapsed_ns(csr_start, csr_end);
        require(csr_ns < sql_ns, "CSR memory lookup was not faster than SQL in graph test fixture");

        const std::vector<codegraph::NodeId> graph_symbols =
            codegraph::graph_symbols_by_name_hash(graph, symbol_name);
        const int64_t sql_symbols = storage.query_int(
            "SELECT COUNT(*) FROM symbols WHERE qualified_name = 'graph_target::anchor';"
        );
        require(static_cast<int64_t>(graph_symbols.size()) == sql_symbols,
                "symbol_by_namehash did not match SQL symbol count");

        size_t graph_lookup_total = 0;
        const auto graph_lookup_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kRepetitions; ++i) {
            graph_lookup_total += codegraph::graph_symbols_by_name_hash(graph, symbol_name).size();
        }
        const auto graph_lookup_end = std::chrono::steady_clock::now();

        uint32_t sql_lookup_total = 0;
        const auto sql_lookup_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kRepetitions; ++i) {
            sql_lookup_total += sql_symbol_lookup_count(storage, symbol_name);
        }
        const auto sql_lookup_end = std::chrono::steady_clock::now();

        uint32_t rg_lookup_total = 0;
        const auto rg_lookup_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kRepetitions; ++i) {
            rg_lookup_total += rg_text_count(repo_root, symbol_name);
        }
        const auto rg_lookup_end = std::chrono::steady_clock::now();

        require(static_cast<int64_t>(graph_lookup_total) == sql_symbols * kRepetitions,
                "graph symbol lookup repeated count did not match SQL");
        require(static_cast<int64_t>(sql_lookup_total) == sql_symbols * kRepetitions,
                "SQL symbol lookup repeated count did not match expected count");
        require(rg_lookup_total >= graph_lookup_total,
                "rg text lookup returned fewer matches than exact graph lookup in fixture");

        const uint64_t graph_lookup_ns = elapsed_ns(graph_lookup_start, graph_lookup_end);
        const uint64_t sql_lookup_ns = elapsed_ns(sql_lookup_start, sql_lookup_end);
        const uint64_t rg_lookup_ns = elapsed_ns(rg_lookup_start, rg_lookup_end);
        require(graph_lookup_ns < sql_lookup_ns,
                "graph symbol lookup was not faster than SQL in graph test fixture");
        require(graph_lookup_ns < rg_lookup_ns,
                "graph symbol lookup was not faster than rg text search in graph test fixture");

        bool found_file = false;
        for (const auto& [path_id, file_id] : graph.file_by_path) {
            (void)file_id;
            if (graph.interner.view(path_id) == target_ref) {
                found_file = true;
                break;
            }
        }
        require(found_file, "file_by_path did not include graph fixture path");

        std::cout << "sql_ns_total: " << sql_ns << "\n";
        std::cout << "csr_ns_total: " << csr_ns << "\n";
        std::cout << "graph_lookup_ns_total: " << graph_lookup_ns << "\n";
        std::cout << "sql_lookup_ns_total: " << sql_lookup_ns << "\n";
        std::cout << "rg_lookup_ns_total: " << rg_lookup_ns << "\n";
    }

    std::filesystem::remove(db_path, ignored);

    std::cout << "graph build: ok\n";
    std::cout << "csr reverse memory: ok\n";
    std::cout << "sorted symbol index: ok\n";
    std::cout << "rg lookup comparison: ok\n";
    std::cout << "sorted file index: ok\n";
    return 0;
}

int command_test_mcp() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    std::error_code exe_ec;
    std::filesystem::path exe_path = std::filesystem::read_symlink("/proc/self/exe", exe_ec);
    if (exe_ec || exe_path.empty()) {
        // /proc/self/exe is Linux-only; fall back to the built binary location.
        exe_path = repo_root / "build" / "codegraph";
    }
    const std::filesystem::path test_repo = repo_root / "build" / "codegraph-test-mcp-repo";
    const std::filesystem::path fixture_dir = test_repo / "testing";
    const std::filesystem::path fixture_file = fixture_dir / "codegraph_mcp_target.cpp";
    const std::filesystem::path codegraph_dir = test_repo / ".codegraph";
    const std::filesystem::path script_path = test_repo / "mcp-script.jsonl";
    const std::filesystem::path stdout_path = test_repo / "mcp-stdout.jsonl";
    const std::filesystem::path stderr_path = test_repo / "mcp-stderr.log";

    std::error_code ignored;
    std::filesystem::remove_all(test_repo, ignored);
    RemoveTreeOnExit remove_test_repo{test_repo};
    std::filesystem::create_directories(fixture_dir);
    std::filesystem::create_directories(test_repo / "docs");

    write_file(
        fixture_file,
        "namespace mcpstep {\n"
        "int target() {\n"
        "    return 9;\n"
        "}\n"
        "}\n"
    );

    // A function defined in an anonymous namespace must still be discoverable by
    // its enclosing named-namespace qualified name (anon-namespace extraction).
    write_file(
        fixture_dir / "codegraph_anon.cpp",
        "namespace mcpstep {\n"
        "namespace {\n"
        "int anon_helper() {\n"
        "    return 7;\n"
        "}\n"
        "}\n"
        "}\n"
    );

    // A non-code file that exists on disk gets a lightweight node, so a memory
    // affecting it must resolve (coverage honesty for unindexed paths).
    write_file(test_repo / "docs" / "NOTES.md", "# Notes\n\nProject notes.\n");

    {
        std::ofstream script(script_path);
        script << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})" << "\n";
        script << R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"find_symbol","arguments":{"name":"mcpstep::target","limit":5}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_symbol","arguments":{"query":"mcpstep::target","include_memory":true}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_memory_for_file","arguments":{"path":"testing/codegraph_mcp_target.cpp"}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"search_symbol","arguments":{"query":"target:* -(","kind":"function","limit":10}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"read_enclosing_symbol","arguments":{"path":"testing/codegraph_mcp_target.cpp","line":2}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"record_correction","arguments":{"reason":"MCP correction reason","affects":["testing/codegraph_mcp_target.cpp"],"prefer_paths":["testing/**"]}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"get_memory_for_file","arguments":{"path":"testing/codegraph_mcp_target.cpp"}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"read_symbol","arguments":{"query":"mcpstep::target","include_memory":true}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"list_symbols_in_file","arguments":{"path":"testing/codegraph_mcp_target.cpp"}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"record_decision","arguments":{"title":"Untracked target decision","body":"affects a path that is not indexed","affects":["docs/UNTRACKED.md"]}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"find_symbol","arguments":{"name":"mcpstep::anon_helper","limit":5}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"record_decision","arguments":{"title":"Notes decision","body":"affects an existing markdown file","affects":["docs/NOTES.md"]}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"find_prior_incidents","arguments":{"query":"correction reason","limit":10}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"write_handoff","arguments":{"title":"MCP handoff","body":"handoff affecting the fixture file","affects":["testing/codegraph_mcp_target.cpp"]}}})" << "\n";
        script << R"({"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"get_memory_for_file","arguments":{"path":"testing/codegraph_mcp_target.cpp"}}})" << "\n";
    }

    const std::string command =
        "cd " + shell_quote(test_repo.string()) + " && " +
        shell_quote(exe_path.string()) + " mcp < " + shell_quote(script_path.string()) +
        " > " + shell_quote(stdout_path.string()) +
        " 2> " + shell_quote(stderr_path.string());
    const int rc = std::system(command.c_str());
    require(rc == 0, "codegraph mcp subprocess failed");

    std::ifstream stdout_file(stdout_path);
    require(static_cast<bool>(stdout_file), "mcp stdout file was not created");
    std::vector<nlohmann::json> responses;
    std::string line;
    while (std::getline(stdout_file, line)) {
        if (line.empty()) {
            continue;
        }
        responses.push_back(nlohmann::json::parse(line));
    }
    require(responses.size() == 17U, "mcp stdout did not contain expected JSON-RPC responses only");

    auto response_by_id = [&](int id) -> nlohmann::json {
        for (const nlohmann::json& response : responses) {
            if (response.value("id", -1) == id) {
                return response;
            }
        }
        throw std::runtime_error("missing MCP response id " + std::to_string(id));
    };
    auto tool_text = [&](int id) -> nlohmann::json {
        const nlohmann::json response = response_by_id(id);
        require(!response.contains("error"), "mcp tool response had protocol error");
        const std::string text = response["result"]["content"][0]["text"].get<std::string>();
        return nlohmann::json::parse(text);
    };

    require(response_by_id(1)["result"]["capabilities"].contains("tools"), "mcp initialize missing tools");
    require(response_by_id(2)["result"]["tools"].size() >= 8U, "mcp tools/list returned too few tools");
    require(std::filesystem::exists(codegraph_dir / "config.yaml"), "mcp did not self-bootstrap config");
    require(std::filesystem::exists(codegraph_dir / "device_id"), "mcp did not self-bootstrap device id");
    require(!tool_text(3).empty(), "mcp find_symbol returned no graph matches");

    const nlohmann::json read = tool_text(4);
    require(read["hash_status"] == "ok", "mcp read_symbol did not use verified fast path");
    require(read["body"].get<std::string>().find("return 9") != std::string::npos,
            "mcp read_symbol returned wrong body");

    require(!tool_text(6).empty(), "mcp search_symbol returned no FTS matches");
    require(
        tool_text(7)["qualified_name"] == "mcpstep::target",
        "mcp read_enclosing_symbol returned wrong symbol"
    );

    const nlohmann::json recorded = tool_text(8);
    require(recorded["node_id"].get<int64_t>() > 0, "mcp record_correction did not return node_id");
    // The affected file is indexed, so a write that fully resolves must report
    // an empty unresolved_affects rather than leaving the caller to infer it.
    require(recorded.contains("unresolved_affects") && recorded["unresolved_affects"].empty(),
            "mcp record_correction with an indexed target reported unresolved affects");

    const nlohmann::json followup = tool_text(9);
    bool saw_recorded = false;
    for (const nlohmann::json& correction : followup["corrections"]) {
        if (correction.value("body", "") == "MCP correction reason") {
            saw_recorded = true;
        }
    }
    require(saw_recorded, "mcp graph was not rebuilt after record_correction");
    require(!tool_text(10)["memory"]["corrections"].empty(), "mcp read_symbol did not bundle memory");

    // list_symbols_in_file must recurse the hierarchical Contains edges: a flat
    // top-level walk would return only the namespace and drop mcpstep::target,
    // and parent tracking must point the nested function at its namespace, not
    // the file. Both properties below fail loudly if that regresses.
    const nlohmann::json listed = tool_text(11);
    require(listed["path"] == "testing/codegraph_mcp_target.cpp",
            "mcp list_symbols_in_file returned wrong path");
    require(listed["symbols"].size() == 2U,
            "mcp list_symbols_in_file did not return namespace + nested function");
    require(listed["symbols"][0]["qualified_name"] == "mcpstep",
            "mcp list_symbols_in_file not ordered by start line");
    int64_t namespace_node = -1;
    int64_t target_parent = -2;
    bool saw_target = false;
    for (const nlohmann::json& sym : listed["symbols"]) {
        const std::string qualified = sym.value("qualified_name", "");
        if (qualified == "mcpstep") {
            namespace_node = sym["node_id"].get<int64_t>();
        }
        if (qualified == "mcpstep::target") {
            saw_target = true;
            target_parent = sym["parent_node_id"].get<int64_t>();
        }
    }
    require(saw_target, "mcp list_symbols_in_file dropped the nested symbol");
    require(target_parent == namespace_node,
            "mcp list_symbols_in_file did not nest the symbol under its namespace");

    // A write whose affects target is not an indexed node must surface that
    // target explicitly (coverage honesty), not hide it behind a counter.
    const nlohmann::json untracked = tool_text(12);
    require(untracked["unresolved_affects"].size() == 1U &&
                untracked["unresolved_affects"][0] == "docs/UNTRACKED.md",
            "mcp record_decision did not report the unresolved affects target");

    // Anon-namespace extraction: a function in an anonymous namespace resolves by
    // its enclosing named-namespace qualified name.
    const nlohmann::json anon = tool_text(13);
    require(!anon.empty() && anon[0]["qualified_name"] == "mcpstep::anon_helper",
            "mcp find_symbol did not resolve the anonymous-namespace function");

    // Coverage honesty: a decision affecting an existing .md file fully resolves,
    // because the scanner now gives every text file a lightweight node.
    const nlohmann::json notes = tool_text(14);
    require(notes.contains("unresolved_affects") && notes["unresolved_affects"].empty(),
            "mcp record_decision affecting an existing markdown file reported unresolved affects");

    // find_prior_incidents: FTS over memory bodies returns the recorded correction
    // with provenance (memory_type + affected paths).
    const nlohmann::json incidents = tool_text(15);
    bool saw_incident = false;
    for (const nlohmann::json& incident : incidents) {
        if (incident.value("memory_type", "") == "correction" &&
            incident.value("body", "") == "MCP correction reason") {
            saw_incident = true;
            bool affects_target = false;
            for (const nlohmann::json& affected : incident["affects"]) {
                if (affected == "testing/codegraph_mcp_target.cpp") {
                    affects_target = true;
                }
            }
            require(affects_target, "find_prior_incidents did not report affected path provenance");
        }
    }
    require(saw_incident, "find_prior_incidents did not return the recorded correction");

    // Handoff surfacing: a handoff affecting a file must appear under get_memory_for_file
    // (not only corrections/decisions), snippeted to keep the response cheap.
    const nlohmann::json with_handoff = tool_text(17);
    require(with_handoff.contains("handoffs") && with_handoff["handoffs"].size() == 1U,
            "mcp get_memory_for_file did not surface the affecting handoff");
    require(with_handoff["handoffs"][0]["memory_type"] == "handoff" &&
                with_handoff["handoffs"][0].contains("body_truncated"),
            "mcp get_memory_for_file handoff entry missing memory_type/body_truncated");

    std::cout << "mcp jsonrpc: ok\n";
    std::cout << "mcp tools: ok\n";
    std::cout << "mcp graph reads: ok\n";
    std::cout << "mcp search/write: ok\n";
    return 0;
}

uint64_t output_u64_value(const std::string& output, std::string_view key) {
    const std::string prefix = std::string(key) + ": ";
    const size_t pos = output.find(prefix);
    if (pos == std::string::npos) {
        throw std::runtime_error("missing output key: " + std::string(key) + "\n" + output);
    }
    const size_t start = pos + prefix.size();
    const size_t end = output.find('\n', start);
    return static_cast<uint64_t>(std::stoull(output.substr(start, end - start)));
}

void acceptance_two_op_streams() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    const std::filesystem::path db_path =
        repo_root / "build" / "codegraph-test-acceptance-streams.sqlite";
    const std::filesystem::path codegraph_dir =
        repo_root / "build" / "codegraph-test-acceptance-streams-cg";
    const std::filesystem::path fixture_file =
        repo_root / "testing" / "codegraph_acceptance_stream.cpp";
    const std::string target_ref = "testing/codegraph_acceptance_stream.cpp";

    std::error_code ignored;
    std::filesystem::remove(db_path, ignored);
    std::filesystem::remove_all(codegraph_dir, ignored);
    std::filesystem::remove(fixture_file, ignored);
    RemoveTreeOnExit remove_codegraph_dir{codegraph_dir};
    RemoveOnExit remove_fixture_file{fixture_file};

    write_file(
        fixture_file,
        "namespace acceptance_stream {\n"
        "int target() { return 1; }\n"
        "}\n"
    );

    codegraph::FrontendRegistry registry = make_frontend_registry();
    codegraph::Storage storage(db_path);
    storage.initialize_schema();
    (void)codegraph::scan_repository(storage, registry, codegraph::ScanOptions{repo_root});
    (void)codegraph::index_repository(storage, registry, codegraph::IndexOptions{repo_root});

    std::filesystem::create_directories(codegraph_dir);
    write_file(codegraph_dir / "device_id", "machine-a\n");
    (void)codegraph::append_correction_op(
        codegraph_dir,
        codegraph::CorrectionInput{
            "Machine A correction",
            "Machine A reason",
            {},
            {},
            {target_ref},
        }
    );
    write_file(codegraph_dir / "device_id", "machine-b\n");
    (void)codegraph::append_decision_op(
        codegraph_dir,
        codegraph::DecisionInput{
            "Machine B decision",
            "Machine B body",
            {target_ref},
        }
    );

    const codegraph::MaterializeResult materialized =
        codegraph::materialize(storage, codegraph_dir);
    require(materialized.ops_applied == 2U, "two op streams did not both apply");
    require(storage.query_int("SELECT COUNT(*) FROM memories;") == 2,
            "two op streams did not produce two memories");
    require(storage.query_int("SELECT COUNT(*) FROM op_index;") == 2,
            "two op streams did not produce two op_index rows");
}

void acceptance_doctor_and_bench() {
    const std::filesystem::path repo_root = std::filesystem::current_path();
    std::error_code exe_ec;
    std::filesystem::path exe_path = std::filesystem::read_symlink("/proc/self/exe", exe_ec);
    if (exe_ec || exe_path.empty()) {
        // /proc/self/exe is Linux-only; fall back to the built binary location.
        exe_path = repo_root / "build" / "codegraph";
    }
    const std::filesystem::path test_repo = repo_root / "build" / "codegraph-test-acceptance-repo";
    const std::filesystem::path source_dir = test_repo / "src";
    const std::filesystem::path source_file = source_dir / "accept.cpp";

    std::error_code ignored;
    std::filesystem::remove_all(test_repo, ignored);
    RemoveTreeOnExit remove_test_repo{test_repo};
    std::filesystem::create_directories(source_dir);
    write_file(
        source_file,
        "namespace accept {\n"
        "int target() {\n"
        "    return 1;\n"
        "}\n"
        "}\n"
    );

    const auto run_in_repo = [&](const std::string& command) {
        return run_command_capture(
            "cd " + shell_quote(test_repo.string()) + " && " +
            shell_quote(exe_path.string()) + " " + command + " 2>&1"
        );
    };

    (void)run_command_capture(shell_quote(exe_path.string()) + " init " + shell_quote(test_repo.string()) + " 2>&1");
    (void)run_in_repo("correct --reason " + shell_quote("Acceptance memory") +
                      " --affects src/accept.cpp --prefer " + shell_quote("src/**"));
    const std::string doctor = run_command_capture(
        shell_quote(exe_path.string()) + " doctor " + shell_quote(test_repo.string()) + " 2>&1"
    );
    require(doctor.find("doctor: ok") != std::string::npos, "doctor did not pass acceptance repo\n" + doctor);

    constexpr uint64_t kSymbolLookupNs = 10'000'000ULL;
    constexpr uint64_t kMemoryForNs = 20'000'000ULL;
    constexpr uint64_t kReadNs = 10'000'000ULL;
    constexpr uint64_t kIndexNs = 100'000'000ULL;
    constexpr uint64_t kRepetitions = 10ULL;

    const std::string lookup = run_in_repo("bench lookup accept::target 10");
    require(output_u64_value(lookup, "graph_ns_total") / kRepetitions < kSymbolLookupNs,
            "bench lookup missed symbol lookup target\n" + lookup);

    const std::string memory = run_in_repo("bench memory-for src/accept.cpp 10");
    require(output_u64_value(memory, "csr_ns_total") / kRepetitions < kMemoryForNs,
            "bench memory-for missed memory target\n" + memory);

    const std::string read = run_in_repo("bench read accept::target 10");
    require(output_u64_value(read, "ns_total") / kRepetitions < kReadNs,
            "bench read missed exact read target\n" + read);

    write_file(
        source_file,
        "namespace accept {\n"
        "int target() {\n"
        "    return 2;\n"
        "}\n"
        "}\n"
    );
    const std::string index = run_in_repo("bench index 1");
    require(output_u64_value(index, "ns_total") < kIndexNs,
            "bench index missed incremental index target\n" + index);
}

int command_test_acceptance() {
    command_test_scan();
    command_test_index();
    command_test_read();
    command_test_memory_reads();
    command_test_materialize();
    acceptance_two_op_streams();
    acceptance_doctor_and_bench();

    std::cout << "acceptance scan/index: ok\n";
    std::cout << "acceptance exact reads: ok\n";
    std::cout << "acceptance memory/materialize: ok\n";
    std::cout << "acceptance multi-stream ops: ok\n";
    std::cout << "acceptance doctor/bench: ok\n";
    return 0;
}

void print_usage() {
    std::cerr
        << "usage:\n"
        << "  codegraph --version\n"
        << "  codegraph doctor-deps\n"
        << "  codegraph doctor [path]\n"
        << "  codegraph init [path]\n"
        << "  codegraph scan [path]\n"
        << "  codegraph index [path]\n"
        << "  codegraph mcp [path]\n"
        << "  codegraph find-symbol <name>\n"
        << "  codegraph search-symbol <query> [--kind K] [--limit N]\n"
        << "  codegraph read-symbol <name>\n"
        << "  codegraph read-file <path> --start N --end M\n"
        << "  codegraph remember --title T --body B --affects P [--affects P2 ...]\n"
        << "  codegraph correct --reason R --affects P [--prefer G] [--avoid G]\n"
        << "  codegraph materialize\n"
        << "  codegraph memory-for <path-or-symbol>\n"
        << "  codegraph bench lookup|memory-for|read <target> [repetitions]\n"
        << "  codegraph bench index [repetitions]\n"
        << "  codegraph parse-smoke <path>\n"
        << "  codegraph test-core\n"
        << "  codegraph test-storage\n"
        << "  codegraph test-scan\n"
        << "  codegraph test-index\n"
        << "  codegraph test-read\n"
        << "  codegraph test-materialize\n"
        << "  codegraph test-memory\n"
        << "  codegraph test-graph\n"
        << "  codegraph test-mcp\n"
        << "  codegraph test-acceptance\n";
}


}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
            return command_version();
        }

        if (argc == 2 && std::strcmp(argv[1], "doctor-deps") == 0) {
            return command_doctor_deps();
        }

        if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "doctor") == 0) {
            const std::string path = argc == 3 ? argv[2] : "";
            return command_doctor(argc == 3 ? &path : nullptr);
        }

        if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "init") == 0) {
            const std::string path = argc == 3 ? argv[2] : "";
            return command_init(argc == 3 ? &path : nullptr);
        }

        if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "scan") == 0) {
            const std::string path = argc == 3 ? argv[2] : "";
            return command_scan(argc == 3 ? &path : nullptr);
        }

        if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "index") == 0) {
            const std::string path = argc == 3 ? argv[2] : "";
            return command_index(argc == 3 ? &path : nullptr);
        }

        if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "mcp") == 0) {
            const std::string path = argc == 3 ? argv[2] : "";
            const std::filesystem::path repo_root = command_repo_root(argc == 3 ? &path : nullptr);
            const std::filesystem::path codegraph_dir = repo_root / ".codegraph";
            const std::filesystem::path db_path = codegraph_dir / "graph.sqlite";
            std::filesystem::create_directories(codegraph_dir);
            codegraph::Storage storage(db_path);
            storage.initialize_schema();
            codegraph::FrontendRegistry registry = make_frontend_registry();
            if (codegraph::bootstrap_needed(storage, codegraph_dir)) {
                (void)codegraph::bootstrap_repository(storage, registry, repo_root, codegraph_dir);
            }
            const codegraph::RepoConfig config = codegraph::load_or_create_config(repo_root);
            return codegraph::run_mcp_server(
                storage,
                registry,
                codegraph::index_options_for_config(repo_root, config),
                codegraph_dir
            );
        }

        if (argc == 3 && std::strcmp(argv[1], "find-symbol") == 0) {
            return command_find_symbol(argv[2]);
        }

        if (argc >= 3 && std::strcmp(argv[1], "search-symbol") == 0) {
            std::vector<std::string> args;
            for (int i = 2; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return command_search_symbol(args);
        }

        if (argc == 3 && std::strcmp(argv[1], "read-symbol") == 0) {
            return command_read_symbol(argv[2]);
        }

        if (argc == 7 && std::strcmp(argv[1], "read-file") == 0) {
            return command_read_file({argv[2], argv[3], argv[4], argv[5], argv[6]});
        }

        if (argc >= 2 && std::strcmp(argv[1], "materialize") == 0) {
            return command_materialize();
        }

        if (argc >= 2 && std::strcmp(argv[1], "remember") == 0) {
            std::vector<std::string> args;
            for (int i = 2; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return command_remember(args);
        }

        if (argc >= 2 && std::strcmp(argv[1], "correct") == 0) {
            std::vector<std::string> args;
            for (int i = 2; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return command_correct(args);
        }

        if (argc == 3 && std::strcmp(argv[1], "memory-for") == 0) {
            return command_memory_for(argv[2]);
        }

        if (argc >= 3 && std::strcmp(argv[1], "bench") == 0) {
            std::vector<std::string> args;
            for (int i = 2; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return command_bench(args);
        }

        if (argc == 3 && std::strcmp(argv[1], "parse-smoke") == 0) {
            return command_parse_smoke(argv[2]);
        }

        if (argc == 2 && std::strcmp(argv[1], "test-core") == 0) {
            return command_test_core();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-storage") == 0) {
            return command_test_storage();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-scan") == 0) {
            return command_test_scan();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-index") == 0) {
            return command_test_index();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-read") == 0) {
            return command_test_read();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-materialize") == 0) {
            return command_test_materialize();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-memory") == 0) {
            return command_test_memory_reads();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-graph") == 0) {
            return command_test_graph();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-mcp") == 0) {
            return command_test_mcp();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-acceptance") == 0) {
            return command_test_acceptance();
        }

        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
