#include "sqlite_util.h"

#include <limits>
#include <stdexcept>

namespace codegraph {

std::string sqlite_message(sqlite3* db, std::string_view fallback) {
    if (db != nullptr) {
        return sqlite3_errmsg(db);
    }
    return std::string(fallback);
}

void check_sqlite(int rc, sqlite3* db, std::string_view context) {
    if (rc == SQLITE_OK) {
        return;
    }
    throw std::runtime_error(std::string(context) + ": " + sqlite_message(db, "sqlite error"));
}

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
    const std::string query(sql);
    const int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt_, nullptr);
    check_sqlite(rc, db_, "failed to prepare sqlite statement");
}

Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

sqlite3_stmt* Statement::get() const {
    return stmt_;
}

sqlite3* Statement::db() const {
    return db_;
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error(sqlite_message(db_, "sqlite step failed"));
}

void Statement::expect_row(std::string_view context) {
    if (!step()) {
        throw std::runtime_error(std::string(context) + ": sqlite query returned no row");
    }
}

void Statement::expect_done(std::string_view context) {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string(context) + ": " + sqlite_message(db_, "sqlite step failed"));
    }
}

void bind_text(sqlite3_stmt* stmt, int index, std::string_view value) {
    const int rc = sqlite3_bind_text(
        stmt,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    );
    check_sqlite(rc, sqlite3_db_handle(stmt), "failed to bind sqlite text");
}

void bind_int64(sqlite3_stmt* stmt, int index, int64_t value) {
    const int rc = sqlite3_bind_int64(stmt, index, value);
    check_sqlite(rc, sqlite3_db_handle(stmt), "failed to bind sqlite integer");
}

void bind_blob(sqlite3_stmt* stmt, int index, const std::vector<uint8_t>& bytes) {
    const char empty = '\0';
    const void* data = bytes.empty() ? static_cast<const void*>(&empty)
                                     : static_cast<const void*>(bytes.data());
    const int rc = sqlite3_bind_blob(
        stmt,
        index,
        data,
        static_cast<int>(bytes.size()),
        SQLITE_TRANSIENT
    );
    check_sqlite(rc, sqlite3_db_handle(stmt), "failed to bind sqlite blob");
}

std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    if (text == nullptr) {
        return {};
    }
    const int size = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<size_t>(size));
}

uint32_t checked_u32(int64_t value, std::string_view field) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string(field) + " is outside uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

}  // namespace codegraph
