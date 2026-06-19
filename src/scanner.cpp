#include "scanner.h"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "file_util.h"
#include "hash_util.h"
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

bool ignored_relative_path(std::string_view path) {
    // Tier 0 hard-codes the spec defaults; config.yaml loading comes in a later milestone.
    return first_component_is(path, ".git") ||
           first_component_is(path, "build") ||
           first_component_is(path, "node_modules") ||
           first_component_is(path, "third_party") ||
           first_component_is(path, "generated") ||
           first_component_starts_with(path, "cmake-build-") ||
           contains_component(path, "__pycache__");
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

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(select_file.get(), 0));
    const int size = sqlite3_column_bytes(select_file.get(), 0);
    return text != nullptr && std::string_view(text, static_cast<size_t>(size)) == hash;
}

int64_t select_file_id(Statement& select_file_id_stmt, std::string_view path) {
    select_file_id_stmt.reset();
    bind_text(select_file_id_stmt.get(), 1, path);
    select_file_id_stmt.expect_row("select file_id");
    return sqlite3_column_int64(select_file_id_stmt.get(), 0);
}

}  // namespace

bool is_cpp_path(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
           ext == ".h" || ext == ".hpp";
}

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

ScanResult scan_repository(Storage& storage, const ScanOptions& options) {
    const std::filesystem::path repo_root = std::filesystem::weakly_canonical(options.repo_root);
    ScanResult result;
    result.branch = git_output(repo_root, "rev-parse --abbrev-ref HEAD");
    result.commit_hash = git_output(repo_root, "rev-parse HEAD");

    Statement select_file(
        storage.handle(),
        "SELECT content_hash FROM files WHERE path = ?;"
    );
    Statement upsert_file(
        storage.handle(),
        "INSERT INTO files(path, language, content_hash, size_bytes, line_count, commit_hash, indexed_at) "
        "VALUES (?, 'cpp', ?, ?, ?, ?, ?) "
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
    Statement upsert_line_table(
        storage.handle(),
        "INSERT INTO line_tables(file_id, offsets_blob) "
        "VALUES (?, ?) "
        "ON CONFLICT(file_id) DO UPDATE SET offsets_blob = excluded.offsets_blob;"
    );

    storage.execute("BEGIN IMMEDIATE;");
    try {
        for (std::filesystem::recursive_directory_iterator it(
                 repo_root,
                 std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::recursive_directory_iterator();
             ++it) {
            const std::filesystem::path rel_path = std::filesystem::relative(it->path(), repo_root);
            const std::string rel = rel_path.generic_string();

            if (ignored_relative_path(rel)) {
                if (it->is_directory()) {
                    it.disable_recursion_pending();
                }
                continue;
            }

            if (!it->is_regular_file() || !is_cpp_path(it->path())) {
                continue;
            }

            const uintmax_t file_size = it->file_size();
            if (file_size > options.max_file_size_bytes) {
                continue;
            }

            ++result.files_seen;
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
            bind_text(upsert_file.get(), 2, hash);
            bind_int64(upsert_file.get(), 3, static_cast<int64_t>(bytes.size()));
            bind_int64(upsert_file.get(), 4, static_cast<int64_t>(offsets.size()));
            bind_text(upsert_file.get(), 5, result.commit_hash);
            bind_text(upsert_file.get(), 6, indexed_at);
            upsert_file.expect_done("upsert file");

            const int64_t file_id = select_file_id(select_file_id_stmt, rel);
            upsert_line_table.reset();
            bind_int64(upsert_line_table.get(), 1, file_id);
            bind_blob(upsert_line_table.get(), 2, packed_offsets);
            upsert_line_table.expect_done("upsert line table");

            ++result.files_indexed;
            result.bytes_indexed += static_cast<uint64_t>(bytes.size());
        }

        storage.execute("COMMIT;");
    } catch (...) {
        storage.execute("ROLLBACK;");
        throw;
    }

    return result;
}

}  // namespace codegraph
