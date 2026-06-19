#include "storage.h"

#include <stdexcept>
#include <string>

namespace codegraph {
namespace {

std::string sqlite_message(sqlite3* db, const char* fallback) {
    if (db != nullptr) {
        return sqlite3_errmsg(db);
    }
    return fallback;
}

void bind_text(sqlite3_stmt* stmt, int index, std::string_view value) {
    const int rc = sqlite3_bind_text(
        stmt,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    );
    if (rc != SQLITE_OK) {
        throw std::runtime_error("failed to bind sqlite text parameter");
    }
}

constexpr const char* kSchemaSql = R"sql(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS files (
  file_id      INTEGER PRIMARY KEY,
  path         TEXT UNIQUE NOT NULL,
  language     TEXT NOT NULL,
  content_hash TEXT NOT NULL,
  size_bytes   INTEGER NOT NULL,
  line_count   INTEGER NOT NULL,
  commit_hash  TEXT,
  indexed_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS line_tables (
  file_id      INTEGER PRIMARY KEY,
  offsets_blob BLOB NOT NULL,
  FOREIGN KEY(file_id) REFERENCES files(file_id)
);

CREATE TABLE IF NOT EXISTS symbols (
  symbol_id      INTEGER PRIMARY KEY,
  file_id        INTEGER NOT NULL,
  kind           TEXT NOT NULL,
  name           TEXT NOT NULL,
  qualified_name TEXT NOT NULL,
  signature      TEXT,
  start_line     INTEGER NOT NULL,
  end_line       INTEGER NOT NULL,
  start_byte     INTEGER NOT NULL,
  end_byte       INTEGER NOT NULL,
  content_hash   TEXT NOT NULL,
  commit_hash    TEXT,
  FOREIGN KEY(file_id) REFERENCES files(file_id)
);
CREATE INDEX IF NOT EXISTS idx_sym_name ON symbols(name);
CREATE INDEX IF NOT EXISTS idx_sym_qual ON symbols(qualified_name);
CREATE INDEX IF NOT EXISTS idx_sym_file ON symbols(file_id);

CREATE TABLE IF NOT EXISTS nodes (
  node_id    INTEGER PRIMARY KEY,
  stable_id  TEXT UNIQUE NOT NULL,
  kind       TEXT NOT NULL,
  title      TEXT NOT NULL,
  created_at TEXT NOT NULL,
  status     TEXT NOT NULL DEFAULT 'active'
);
CREATE INDEX IF NOT EXISTS idx_nodes_kind ON nodes(kind);

CREATE TABLE IF NOT EXISTS edges (
  edge_id   INTEGER PRIMARY KEY,
  from_node INTEGER NOT NULL,
  to_node   INTEGER,
  to_ref    TEXT,
  kind      TEXT NOT NULL,
  resolved  INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY(from_node) REFERENCES nodes(node_id)
);
CREATE INDEX IF NOT EXISTS idx_edges_from       ON edges(from_node);
CREATE INDEX IF NOT EXISTS idx_edges_to         ON edges(to_node);
CREATE INDEX IF NOT EXISTS idx_edges_unresolved ON edges(resolved);

CREATE TABLE IF NOT EXISTS memories (
  memory_id   INTEGER PRIMARY KEY,
  node_id     INTEGER NOT NULL,
  memory_type TEXT NOT NULL,
  title       TEXT NOT NULL,
  body        TEXT NOT NULL,
  created_at  TEXT NOT NULL,
  FOREIGN KEY(node_id) REFERENCES nodes(node_id)
);

CREATE TABLE IF NOT EXISTS path_rules (
  rule_id   INTEGER PRIMARY KEY,
  node_id   INTEGER NOT NULL,
  rule_kind TEXT NOT NULL,
  pattern   TEXT NOT NULL,
  reason    TEXT,
  FOREIGN KEY(node_id) REFERENCES nodes(node_id)
);

CREATE TABLE IF NOT EXISTS op_index (
  op_id      TEXT PRIMARY KEY,
  device_id  TEXT NOT NULL,
  lamport    INTEGER NOT NULL,
  op_type    TEXT NOT NULL,
  applied_at TEXT NOT NULL
);

CREATE VIRTUAL TABLE IF NOT EXISTS fts_symbols USING fts5(
  name,
  qualified_name,
  signature,
  content='symbols',
  content_rowid='symbol_id'
);

CREATE VIRTUAL TABLE IF NOT EXISTS fts_memories USING fts5(
  title,
  body,
  content='memories',
  content_rowid='memory_id'
);

CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN
  INSERT INTO fts_symbols(rowid, name, qualified_name, signature)
  VALUES (new.symbol_id, new.name, new.qualified_name, new.signature);
END;

CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN
  INSERT INTO fts_symbols(fts_symbols, rowid, name, qualified_name, signature)
  VALUES ('delete', old.symbol_id, old.name, old.qualified_name, old.signature);
END;

CREATE TRIGGER IF NOT EXISTS symbols_au AFTER UPDATE ON symbols BEGIN
  INSERT INTO fts_symbols(fts_symbols, rowid, name, qualified_name, signature)
  VALUES ('delete', old.symbol_id, old.name, old.qualified_name, old.signature);
  INSERT INTO fts_symbols(rowid, name, qualified_name, signature)
  VALUES (new.symbol_id, new.name, new.qualified_name, new.signature);
END;

CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN
  INSERT INTO fts_memories(rowid, title, body)
  VALUES (new.memory_id, new.title, new.body);
END;

CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN
  INSERT INTO fts_memories(fts_memories, rowid, title, body)
  VALUES ('delete', old.memory_id, old.title, old.body);
END;

CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN
  INSERT INTO fts_memories(fts_memories, rowid, title, body)
  VALUES ('delete', old.memory_id, old.title, old.body);
  INSERT INTO fts_memories(rowid, title, body)
  VALUES (new.memory_id, new.title, new.body);
END;

PRAGMA user_version = 1;
)sql";

}  // namespace

Storage::Storage(const std::filesystem::path& db_path) {
    const int rc = sqlite3_open(db_path.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string message = sqlite_message(db_, "sqlite3_open failed");
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(message);
    }

    execute("PRAGMA foreign_keys = ON;");
}

Storage::~Storage() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

Storage::Storage(Storage&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Storage& Storage::operator=(Storage&& other) noexcept {
    if (this != &other) {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Storage::initialize_schema() {
    execute(kSchemaSql);
}

void Storage::execute(std::string_view sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : sqlite_message(db_, "sqlite exec failed");
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

bool Storage::object_exists(std::string_view type, std::string_view name) const {
    static constexpr const char* kSql =
        "SELECT COUNT(*) FROM sqlite_master WHERE type = ? AND name = ?;";

    sqlite3_stmt* stmt = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr);
    if (prepare_rc != SQLITE_OK) {
        throw std::runtime_error(sqlite_message(db_, "failed to prepare sqlite query"));
    }

    try {
        bind_text(stmt, 1, type);
        bind_text(stmt, 2, name);

        const int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_ROW) {
            throw std::runtime_error(sqlite_message(db_, "sqlite query returned no row"));
        }

        const bool exists = sqlite3_column_int64(stmt, 0) > 0;
        sqlite3_finalize(stmt);
        return exists;
    } catch (...) {
        sqlite3_finalize(stmt);
        throw;
    }
}

int64_t Storage::query_int(std::string_view sql) const {
    sqlite3_stmt* stmt = nullptr;
    const std::string query(sql);
    const int prepare_rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (prepare_rc != SQLITE_OK) {
        throw std::runtime_error(sqlite_message(db_, "failed to prepare sqlite query"));
    }

    const int step_rc = sqlite3_step(stmt);
    if (step_rc != SQLITE_ROW) {
        const std::string message = sqlite_message(db_, "sqlite query returned no row");
        sqlite3_finalize(stmt);
        throw std::runtime_error(message);
    }

    const int64_t value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

}  // namespace codegraph
