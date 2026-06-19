#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core.h"
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

    require(storage.query_int("PRAGMA user_version;") == 1, "unexpected sqlite user_version");
}


int command_version() {
    std::cout << "codegraph " << kVersion << "\n";
    return 0;
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

    std::cout << "core types: ok\n";
    std::cout << "string interner: ok\n";
    std::cout << "source span pack/unpack: ok\n";
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

void print_usage() {
    std::cerr
        << "usage:\n"
        << "  codegraph --version\n"
        << "  codegraph doctor-deps\n"
        << "  codegraph parse-smoke <path>\n"
        << "  codegraph test-core\n"
        << "  codegraph test-storage\n";
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

        if (argc == 3 && std::strcmp(argv[1], "parse-smoke") == 0) {
            return command_parse_smoke(argv[2]);
        }

        if (argc == 2 && std::strcmp(argv[1], "test-core") == 0) {
            return command_test_core();
        }

        if (argc == 2 && std::strcmp(argv[1], "test-storage") == 0) {
            return command_test_storage();
        }

        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
