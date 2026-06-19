#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace codegraph {

std::string sqlite_message(sqlite3* db, std::string_view fallback);
void check_sqlite(int rc, sqlite3* db, std::string_view context);

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    sqlite3_stmt* get() const;
    sqlite3* db() const;

    void reset();
    bool step();
    void expect_row(std::string_view context);
    void expect_done(std::string_view context);

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void bind_text(sqlite3_stmt* stmt, int index, std::string_view value);
void bind_int64(sqlite3_stmt* stmt, int index, int64_t value);
void bind_blob(sqlite3_stmt* stmt, int index, const std::vector<uint8_t>& bytes);
std::string column_text(sqlite3_stmt* stmt, int column);
uint32_t checked_u32(int64_t value, std::string_view field);

}  // namespace codegraph
