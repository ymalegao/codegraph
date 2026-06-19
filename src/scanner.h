#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "storage.h"

namespace codegraph {

struct ScanOptions {
    std::filesystem::path repo_root;
    size_t max_file_size_bytes = 10U * 1024U * 1024U;
};

struct ScanResult {
    uint32_t files_seen = 0;
    uint32_t files_indexed = 0;
    uint32_t files_unchanged = 0;
    uint64_t bytes_indexed = 0;
    std::string branch;
    std::string commit_hash;
};

bool is_cpp_path(const std::filesystem::path& path);
std::vector<uint32_t> build_line_offsets(std::string_view bytes);
std::vector<uint8_t> pack_line_offsets(const std::vector<uint32_t>& offsets);
std::vector<uint32_t> unpack_line_offsets(const std::vector<uint8_t>& bytes);

ScanResult scan_repository(Storage& storage, const ScanOptions& options);

}  // namespace codegraph
