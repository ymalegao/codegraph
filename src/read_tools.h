#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "frontend.h"
#include "indexer.h"
#include "storage.h"

namespace codegraph {

enum class ReadStatus {
    Ok,
    ReResolved,
    Gone,
    NotFound,
};

struct SymbolMatch {
    int64_t symbol_id = 0;
    std::string path;
    std::string kind;
    std::string name;
    std::string qualified_name;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;
};

struct SymbolSearchMatch {
    int64_t symbol_id = 0;
    std::string qualified_name;
    std::string path;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    std::string kind;
    std::string signature;
    double score = 0.0;
};

struct ReadSymbolResult {
    ReadStatus status = ReadStatus::NotFound;
    SymbolMatch symbol;
    std::string body;
    std::string message;
};

struct FileRangeResult {
    std::string path;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    std::string content_hash;
    std::string text;
};

std::string_view read_status_name(ReadStatus status);

std::vector<SymbolMatch> find_symbols(
    Storage& storage,
    std::string_view query,
    uint32_t limit = 20
);

std::vector<SymbolSearchMatch> search_symbols(
    Storage& storage,
    std::string_view query,
    std::optional<std::string_view> kind = std::nullopt,
    uint32_t limit = 20
);

ReadSymbolResult read_symbol_verified(
    Storage& storage,
    const FrontendRegistry& registry,
    const IndexOptions& options,
    std::string_view query
);

FileRangeResult read_file_range(
    const std::filesystem::path& repo_root,
    std::string_view path,
    uint32_t start_line,
    uint32_t end_line
);

}  // namespace codegraph
