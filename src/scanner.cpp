#include "scanner.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include "core.h"
#include "file_util.h"
#include "hash_util.h"
#include "resolver.h"
#include "source_projection.h"
#include "sqlite_util.h"
#include "time_util.h"

namespace codegraph {
namespace {

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

std::string trim_trailing_newlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string read_command_output(const std::string& command) {
    std::array<char, 256> buffer{};
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
        throw std::runtime_error("command failed: " + command);
    }

    return trim_trailing_newlines(output);
}

std::string git_output(const std::filesystem::path& repo_root, std::string_view args) {
    const std::string command =
        "git -C " + shell_quote(repo_root.string()) + " " + std::string(args) + " 2>/dev/null";
    return read_command_output(command);
}

bool first_component_is(std::string_view path, std::string_view name) {
    return path == name || (path.size() > name.size() &&
                            path.substr(0, name.size()) == name &&
                            path[name.size()] == '/');
}

bool first_component_starts_with(std::string_view path, std::string_view prefix) {
    const size_t slash = path.find('/');
    const std::string_view first = path.substr(0, slash);
    return first.size() >= prefix.size() && first.substr(0, prefix.size()) == prefix;
}

bool contains_component(std::string_view path, std::string_view component) {
    if (path == component) {
        return true;
    }
    const std::string needle = "/" + std::string(component) + "/";
    if (path.find(needle) != std::string_view::npos) {
        return true;
    }
    const std::string prefix = std::string(component) + "/";
    if (path.substr(0, prefix.size()) == prefix) {
        return true;
    }
    const std::string suffix = "/" + std::string(component);
    return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
}

bool matches_ignore_pattern(std::string_view path, std::string_view pattern) {
    if (pattern == ".git/**") return first_component_is(path, ".git");
    if (pattern == "build/**") return first_component_is(path, "build");
    if (pattern == "node_modules/**") return first_component_is(path, "node_modules");
    if (pattern == "third_party/**") return first_component_is(path, "third_party");
    if (pattern == "generated/**") return first_component_is(path, "generated");
    if (pattern == "cmake-build-*/**") return first_component_starts_with(path, "cmake-build-");
    if (pattern == "**/__pycache__/**") return contains_component(path, "__pycache__");
    if (pattern.size() > 3U && pattern.substr(pattern.size() - 3U) == "/**") {
        return first_component_is(path, pattern.substr(0, pattern.size() - 3U));
    }
    return path == pattern;
}

bool ignored_relative_path(std::string_view path, const std::vector<std::string>& patterns) {
    for (const std::string& pattern : patterns) {
        if (matches_ignore_pattern(path, pattern)) {
            return true;
        }
    }
    return false;
}

void write_le32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

uint32_t read_le32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

bool existing_hash_matches(Statement& select_file, std::string_view path, std::string_view hash) {
    select_file.reset();
    bind_text(select_file.get(), 1, path);

    if (!select_file.step()) {
        return false;
    }

    return column_text(select_file.get(), 0) == hash;
}

int64_t select_file_id(Statement& select_file_id_stmt, std::string_view path) {
    select_file_id_stmt.reset();
    bind_text(select_file_id_stmt.get(), 1, path);
    select_file_id_stmt.expect_row("select file_id");
    return sqlite3_column_int64(select_file_id_stmt.get(), 0);
}

// Cheap binary sniff: a NUL byte in the first 8 KB means we treat the file as
// binary and skip it. Used to decide whether a non-frontend file is worth a
// lightweight node (so memories can attach to text files of any kind).
bool looks_binary(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return true;
    }
    std::array<char, 8192> buffer{};
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = stream.gcount();
    for (std::streamsize i = 0; i < read; ++i) {
        if (buffer[static_cast<size_t>(i)] == '\0') {
            return true;
        }
    }
    return false;
}

void prune_unseen_files(
    Storage& storage,
    const FrontendRegistry& registry,
    std::string_view repo_id,
    const std::unordered_set<std::string>& seen_paths,
    ScanResult& result
) {
    Statement stmt(
        storage.handle(),
        "SELECT file_id, path, language FROM files ORDER BY path;"
    );

    struct StaleFile {
        int64_t file_id = 0;
        std::string path;
    };
    std::vector<StaleFile> stale_files;

    while (stmt.step()) {
        const std::string path = column_text(stmt.get(), 1);
        const std::string language = column_text(stmt.get(), 2);
        if (registry.for_language(language) != nullptr && seen_paths.find(path) == seen_paths.end()) {
            stale_files.push_back(StaleFile{sqlite3_column_int64(stmt.get(), 0), path});
        }
    }

    for (const StaleFile& file : stale_files) {
        delete_source_file_projection(storage.handle(), repo_id, file.file_id, file.path, true);
        ++result.files_pruned;
    }
}

// Lightweight file nodes (text files without a frontend) have a node but no
// `files` row. When such a file disappears we drop its node, but first reset any
// memory `affects` edges that pointed at it back to unresolved so we never leave
// a resolved edge dangling (mirrors delete_source_file_projection).
void prune_unseen_lightweight_file_nodes(
    Storage& storage,
    const std::unordered_set<std::string>& seen_paths,
    ScanResult& result
) {
    Statement stmt(
        storage.handle(),
        "SELECT node_id, title FROM nodes "
        "WHERE kind = 'file' AND title NOT IN (SELECT path FROM files);"
    );

    struct StaleNode {
        int64_t node_id = 0;
        std::string title;
    };
    std::vector<StaleNode> stale_nodes;

    while (stmt.step()) {
        const std::string title = column_text(stmt.get(), 1);
        if (seen_paths.find(title) == seen_paths.end()) {
            stale_nodes.push_back(StaleNode{sqlite3_column_int64(stmt.get(), 0), title});
        }
    }

    Statement reset_affects(
        storage.handle(),
        "UPDATE edges SET to_node = NULL, resolved = 0 "
        "WHERE kind = 'affects' AND to_node = ?;"
    );
    Statement delete_node(storage.handle(), "DELETE FROM nodes WHERE node_id = ?;");

    for (const StaleNode& node : stale_nodes) {
        reset_affects.reset();
        bind_int64(reset_affects.get(), 1, node.node_id);
        reset_affects.expect_done("reset affects to stale lightweight node");

        delete_node.reset();
        bind_int64(delete_node.get(), 1, node.node_id);
        delete_node.expect_done("delete stale lightweight file node");
        ++result.files_pruned;
    }
}

}  // namespace

std::vector<uint32_t> build_line_offsets(std::string_view bytes) {
    std::vector<uint32_t> offsets{0};
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\n' && i + 1U < bytes.size()) {
            if (i + 1U > std::numeric_limits<uint32_t>::max()) {
                throw std::length_error("line offset exceeds uint32_t range");
            }
            offsets.push_back(static_cast<uint32_t>(i + 1U));
        }
    }
    return offsets;
}

std::vector<uint8_t> pack_line_offsets(const std::vector<uint32_t>& offsets) {
    std::vector<uint8_t> bytes;
    bytes.reserve(offsets.size() * 4U);
    for (const uint32_t offset : offsets) {
        write_le32(bytes, offset);
    }
    return bytes;
}

std::vector<uint32_t> unpack_line_offsets(const std::vector<uint8_t>& bytes) {
    if (bytes.size() % 4U != 0U) {
        throw std::runtime_error("packed line table length is not divisible by 4");
    }

    std::vector<uint32_t> offsets;
    offsets.reserve(bytes.size() / 4U);
    for (size_t i = 0; i < bytes.size(); i += 4U) {
        offsets.push_back(read_le32(bytes, i));
    }
    return offsets;
}

ScanResult scan_repository(
    Storage& storage,
    const FrontendRegistry& registry,
    const ScanOptions& options
) {
    const std::filesystem::path repo_root = std::filesystem::weakly_canonical(options.repo_root);
    const std::vector<std::string> ignore_patterns =
        options.ignore_patterns.empty()
            ? std::vector<std::string>{
                  ".git/**",
                  "build/**",
                  "cmake-build-*/**",
                  "node_modules/**",
                  "**/__pycache__/**",
                  "third_party/**",
                  "generated/**",
              }
            : options.ignore_patterns;
    ScanResult result;
    try {
        result.branch = git_output(repo_root, "rev-parse --abbrev-ref HEAD");
        result.commit_hash = git_output(repo_root, "rev-parse HEAD");
    } catch (const std::exception&) {
        result.branch.clear();
        result.commit_hash.clear();
    }
    std::unordered_set<std::string> seen_paths;

    Statement select_file(
        storage.handle(),
        "SELECT content_hash FROM files WHERE path = ?;"
    );
    Statement upsert_file(
        storage.handle(),
        "INSERT INTO files(path, language, content_hash, size_bytes, line_count, commit_hash, indexed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "language = excluded.language, "
        "content_hash = excluded.content_hash, "
        "size_bytes = excluded.size_bytes, "
        "line_count = excluded.line_count, "
        "commit_hash = excluded.commit_hash, "
        "indexed_at = excluded.indexed_at;"
    );
    Statement select_file_id_stmt(
        storage.handle(),
        "SELECT file_id FROM files WHERE path = ?;"
    );
    Statement upsert_file_node(
        storage.handle(),
        "INSERT INTO nodes(stable_id, kind, title, created_at, status) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(stable_id) DO UPDATE SET "
        "kind = excluded.kind, title = excluded.title, status = excluded.status;"
    );
    Statement upsert_line_table(
        storage.handle(),
        "INSERT INTO line_tables(file_id, offsets_blob) "
        "VALUES (?, ?) "
        "ON CONFLICT(file_id) DO UPDATE SET offsets_blob = excluded.offsets_blob;"
    );

    storage.execute("BEGIN IMMEDIATE;");
    try {
        const uint32_t foreign_source_nodes =
            prune_source_nodes_for_other_repositories(
            storage.handle(),
            options.repo_id
        );
        if (foreign_source_nodes > 0) {
            // Source node stable IDs include repo_id. If that identity changes,
            // the symbol rows may still match the file hash but their graph nodes
            // were just removed, so force one full projection rebuild.
            storage.execute("UPDATE files SET projection_version = 0;");
        }
        for (std::filesystem::recursive_directory_iterator it(
                 repo_root,
                 std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::recursive_directory_iterator();
             ++it) {
            const std::filesystem::path rel_path = std::filesystem::relative(it->path(), repo_root);
            const std::string rel = rel_path.generic_string();

            if (ignored_relative_path(rel, ignore_patterns)) {
                if (it->is_directory()) {
                    it.disable_recursion_pending();
                }
                continue;
            }

            if (!it->is_regular_file()) {
                continue;
            }

            const uintmax_t file_size = it->file_size();
            if (file_size > options.max_file_size_bytes) {
                continue;
            }

            const LanguageFrontend* frontend = registry.for_extension(it->path().extension().string());
            if (frontend == nullptr) {
                // No frontend: still give text files a lightweight node so memories
                // can attach to any path (docs, configs). No files row, line table,
                // or symbols — there is nothing to extract.
                if (looks_binary(it->path())) {
                    continue;
                }
                seen_paths.insert(rel);
                upsert_file_node.reset();
                bind_text(upsert_file_node.get(), 1, file_stable_id(options.repo_id, rel));
                bind_text(upsert_file_node.get(), 2, node_kind_text(NodeKind::File));
                bind_text(upsert_file_node.get(), 3, rel);
                bind_text(upsert_file_node.get(), 4, current_utc_timestamp());
                bind_text(upsert_file_node.get(), 5, status_text(Status::Active));
                upsert_file_node.expect_done("upsert lightweight file node");
                continue;
            }

            ++result.files_seen;
            seen_paths.insert(rel);

            upsert_file_node.reset();
            bind_text(upsert_file_node.get(), 1, file_stable_id(options.repo_id, rel));
            bind_text(upsert_file_node.get(), 2, node_kind_text(NodeKind::File));
            bind_text(upsert_file_node.get(), 3, rel);
            bind_text(upsert_file_node.get(), 4, current_utc_timestamp());
            bind_text(upsert_file_node.get(), 5, status_text(Status::Active));
            upsert_file_node.expect_done("upsert file node");

            const std::string bytes = read_file_bytes(it->path());
            const std::string hash = xxh64_hex(bytes);
            if (existing_hash_matches(select_file, rel, hash)) {
                ++result.files_unchanged;
                continue;
            }

            const std::vector<uint32_t> offsets = build_line_offsets(bytes);
            const std::vector<uint8_t> packed_offsets = pack_line_offsets(offsets);
            const std::string indexed_at = current_utc_timestamp();

            upsert_file.reset();
            bind_text(upsert_file.get(), 1, rel);
            bind_text(upsert_file.get(), 2, frontend->language());
            bind_text(upsert_file.get(), 3, hash);
            bind_int64(upsert_file.get(), 4, static_cast<int64_t>(bytes.size()));
            bind_int64(upsert_file.get(), 5, static_cast<int64_t>(offsets.size()));
            bind_text(upsert_file.get(), 6, result.commit_hash);
            bind_text(upsert_file.get(), 7, indexed_at);
            upsert_file.expect_done("upsert file");

            const int64_t file_id = select_file_id(select_file_id_stmt, rel);
            upsert_line_table.reset();
            bind_int64(upsert_line_table.get(), 1, file_id);
            bind_blob(upsert_line_table.get(), 2, packed_offsets);
            upsert_line_table.expect_done("upsert line table");

            ++result.files_indexed;
            result.bytes_indexed += static_cast<uint64_t>(bytes.size());
        }

        prune_unseen_files(storage, registry, options.repo_id, seen_paths, result);
        prune_unseen_lightweight_file_nodes(storage, seen_paths, result);
        (void)resolver_pass(storage);
        storage.execute("COMMIT;");
    } catch (...) {
        if (sqlite3_get_autocommit(storage.handle()) == 0) {
            storage.execute("ROLLBACK;");
        }
        throw;
    }

    return result;
}

}  // namespace codegraph
