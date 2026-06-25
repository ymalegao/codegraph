#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace codegraph {

class Storage {
public:
    explicit Storage(const std::filesystem::path& db_path);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    Storage(Storage&& other) noexcept;
    Storage& operator=(Storage&& other) noexcept;

    void initialize_schema();
    void execute(std::string_view sql);
    bool object_exists(std::string_view type, std::string_view name) const;
    int64_t query_int(std::string_view sql) const;
    std::vector<uint8_t> query_blob(std::string_view sql) const;
    sqlite3* handle() const;

private:
    // Migrate a v1 schema to v2: add symbols.body and rebuild fts_symbols so it
    // indexes bodies. Bodies stay empty until the next reindex repopulates them.
    void migrate_v1_to_v2();

    sqlite3* db_ = nullptr;
};

}  // namespace codegraph
