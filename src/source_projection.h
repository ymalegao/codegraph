#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace codegraph {

std::string file_stable_id(std::string_view repo_id, std::string_view path);
std::string symbol_stable_id(std::string_view repo_id, std::string_view path, std::string_view qualified_name);
std::string symbol_stable_prefix(std::string_view repo_id, std::string_view path);

void delete_source_file_projection(
    sqlite3* db,
    std::string_view repo_id,
    int64_t file_id,
    std::string_view path,
    bool delete_file_row
);

}  // namespace codegraph
