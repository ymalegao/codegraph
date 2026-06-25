#include "read_tools.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

#include "file_util.h"
#include "core.h"
#include "hash_util.h"
#include "scanner.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

struct StoredSymbol {
    SymbolMatch match;
    int64_t file_id = 0;
    std::string language;
    std::string file_hash;
};

SymbolMatch symbol_match_from_statement(sqlite3_stmt* stmt) {
    SymbolMatch match;
    match.symbol_id = sqlite3_column_int64(stmt, 0);
    match.path = column_text(stmt, 1);
    match.kind = column_text(stmt, 2);
    match.name = column_text(stmt, 3);
    match.qualified_name = column_text(stmt, 4);
    match.start_line = checked_u32(sqlite3_column_int64(stmt, 5), "start_line");
    match.end_line = checked_u32(sqlite3_column_int64(stmt, 6), "end_line");
    match.start_byte = checked_u32(sqlite3_column_int64(stmt, 7), "start_byte");
    match.end_byte = checked_u32(sqlite3_column_int64(stmt, 8), "end_byte");
    return match;
}

SymbolSearchMatch symbol_search_match_from_statement(sqlite3_stmt* stmt) {
    SymbolSearchMatch match;
    match.symbol_id = sqlite3_column_int64(stmt, 0);
    match.qualified_name = column_text(stmt, 1);
    match.path = column_text(stmt, 2);
    match.start_line = checked_u32(sqlite3_column_int64(stmt, 3), "start_line");
    match.end_line = checked_u32(sqlite3_column_int64(stmt, 4), "end_line");
    match.kind = column_text(stmt, 5);
    match.signature = column_text(stmt, 6);
    match.score = sqlite3_column_double(stmt, 7);
    return match;
}

bool is_fts_token_char(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
}

std::string fts_quote(std::string_view term) {
    std::string quoted = "\"";
    for (const char ch : term) {
        if (ch == '"') {
            quoted += "\"\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

std::string sanitize_fts_query(std::string_view query) {
    std::vector<std::string> terms;
    std::string current;
    for (const unsigned char ch : query) {
        if (is_fts_token_char(ch)) {
            current.push_back(static_cast<char>(ch));
            continue;
        }
        if (!current.empty()) {
            terms.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        terms.push_back(std::move(current));
    }

    // OR the terms with prefix matching so intent phrases ("register mcp tool")
    // rank by how many tokens hit instead of requiring every token to be present.
    // bm25 column weights keep name/qualified_name matches ahead of body matches.
    std::string sanitized;
    for (const std::string& term : terms) {
        if (!sanitized.empty()) {
            sanitized += " OR ";
        }
        sanitized += fts_quote(term);
        sanitized += "*";
    }
    return sanitized;
}

bool lookup_symbol(Storage& storage, std::string_view query, StoredSymbol& symbol) {
    Statement stmt(
        storage.handle(),
        "SELECT s.symbol_id, f.path, s.kind, s.name, s.qualified_name, "
        "s.start_line, s.end_line, s.start_byte, s.end_byte, "
        "f.file_id, f.language, f.content_hash "
        "FROM symbols s JOIN files f ON f.file_id = s.file_id "
        "WHERE s.qualified_name = ? OR s.name = ? "
        "ORDER BY CASE WHEN s.qualified_name = ? THEN 0 ELSE 1 END, f.path, s.start_line "
        "LIMIT 1;"
    );
    bind_text(stmt.get(), 1, query);
    bind_text(stmt.get(), 2, query);
    bind_text(stmt.get(), 3, query);

    if (!stmt.step()) {
        return false;
    }

    symbol.match = symbol_match_from_statement(stmt.get());
    symbol.file_id = sqlite3_column_int64(stmt.get(), 9);
    symbol.language = column_text(stmt.get(), 10);
    symbol.file_hash = column_text(stmt.get(), 11);
    return true;
}

bool lookup_symbol_in_file(
    Storage& storage,
    std::string_view path,
    std::string_view qualified_name,
    StoredSymbol& symbol
) {
    Statement stmt(
        storage.handle(),
        "SELECT s.symbol_id, f.path, s.kind, s.name, s.qualified_name, "
        "s.start_line, s.end_line, s.start_byte, s.end_byte, "
        "f.file_id, f.language, f.content_hash "
        "FROM symbols s JOIN files f ON f.file_id = s.file_id "
        "WHERE f.path = ? AND s.qualified_name = ? "
        "ORDER BY s.start_line LIMIT 1;"
    );
    bind_text(stmt.get(), 1, path);
    bind_text(stmt.get(), 2, qualified_name);

    if (!stmt.step()) {
        return false;
    }

    symbol.match = symbol_match_from_statement(stmt.get());
    symbol.file_id = sqlite3_column_int64(stmt.get(), 9);
    symbol.language = column_text(stmt.get(), 10);
    symbol.file_hash = column_text(stmt.get(), 11);
    return true;
}

std::string span_text(std::string_view source, const SymbolMatch& symbol) {
    if (symbol.start_byte > symbol.end_byte || symbol.end_byte > source.size()) {
        throw std::runtime_error("stored symbol span is outside current file bytes");
    }
    return std::string(source.substr(symbol.start_byte, symbol.end_byte - symbol.start_byte));
}

}  // namespace

std::string_view read_status_name(ReadStatus status) {
    switch (status) {
        case ReadStatus::Ok:
            return "ok";
        case ReadStatus::ReResolved:
            return "re_resolved";
        case ReadStatus::Gone:
            return "gone";
        case ReadStatus::NotFound:
            return "not_found";
    }
    return "unknown";
}

std::vector<SymbolMatch> find_symbols(Storage& storage, std::string_view query, uint32_t limit) {
    Statement stmt(
        storage.handle(),
        "SELECT s.symbol_id, f.path, s.kind, s.name, s.qualified_name, "
        "s.start_line, s.end_line, s.start_byte, s.end_byte "
        "FROM symbols s JOIN files f ON f.file_id = s.file_id "
        "WHERE s.qualified_name = ? OR s.name = ? OR s.qualified_name LIKE ? "
        "ORDER BY CASE WHEN s.qualified_name = ? THEN 0 WHEN s.name = ? THEN 1 ELSE 2 END, "
        "f.path, s.start_line "
        "LIMIT ?;"
    );
    const std::string like = "%" + std::string(query) + "%";
    bind_text(stmt.get(), 1, query);
    bind_text(stmt.get(), 2, query);
    bind_text(stmt.get(), 3, like);
    bind_text(stmt.get(), 4, query);
    bind_text(stmt.get(), 5, query);
    bind_int64(stmt.get(), 6, limit);

    std::vector<SymbolMatch> matches;
    while (stmt.step()) {
        matches.push_back(symbol_match_from_statement(stmt.get()));
    }
    return matches;
}

std::vector<SymbolSearchMatch> search_symbols(
    Storage& storage,
    std::string_view query,
    std::optional<std::string_view> kind,
    uint32_t limit
) {
    const std::string match_query = sanitize_fts_query(query);
    if (match_query.empty() || limit == 0) {
        return {};
    }

    std::string normalized_kind;
    if (kind.has_value() && !kind->empty()) {
        normalized_kind = std::string(symbol_kind_text(symbol_kind_from_string(*kind)));
    }

    // Trust contract: OR + prefix matching (sanitize_fts_query) gives intent recall,
    // but a query that grazes one ubiquitous token must not surface false positives —
    // "empty means absent, not missed". Two guards:
    //  - exclude namespace symbols (their FTS body spans the whole file, so they match
    //    almost anything) unless the caller explicitly asks for kind='namespace';
    //  - a RELATIVE relevance floor applied below: keep only results within a factor of
    //    the best score. Relative, not absolute, because bm25 magnitudes are corpus-size
    //    dependent (a legit match in a tiny repo can score near zero).
    Statement stmt(
        storage.handle(),
        "SELECT s.symbol_id, s.qualified_name, f.path, s.start_line, s.end_line, "
        "s.kind, COALESCE(s.signature, ''), bm25(fts_symbols, 10.0, 5.0, 1.0, 0.5) AS score "
        "FROM fts_symbols "
        "JOIN symbols s ON s.symbol_id = fts_symbols.rowid "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE fts_symbols MATCH ? "
        "AND (? = '' OR s.kind = ?) "
        "AND (? != '' OR s.kind != 'namespace') "
        "ORDER BY score, f.path, s.start_line "
        "LIMIT ?;"
    );
    bind_text(stmt.get(), 1, match_query);
    bind_text(stmt.get(), 2, normalized_kind);
    bind_text(stmt.get(), 3, normalized_kind);
    bind_text(stmt.get(), 4, normalized_kind);
    bind_int64(stmt.get(), 5, limit);

    std::vector<SymbolSearchMatch> matches;
    while (stmt.step()) {
        matches.push_back(symbol_search_match_from_statement(stmt.get()));
    }

    // Relative relevance floor: results are ordered best-first (bm25 is
    // more-negative-is-better). Drop those far weaker than the top match so a query
    // that only grazes a common token does not trail a wall of near-zero hits. Using
    // a factor of the best score keeps this corpus-size independent.
    if (!matches.empty()) {
        constexpr double kKeepFactor = 0.05;
        const double cutoff = matches.front().score * kKeepFactor;
        matches.erase(
            std::remove_if(
                matches.begin(), matches.end(),
                [cutoff](const SymbolSearchMatch& match) { return match.score > cutoff; }
            ),
            matches.end()
        );
    }
    return matches;
}

std::vector<MemoryIncidentMatch> find_memory_incidents(
    Storage& storage,
    std::string_view query,
    uint32_t limit
) {
    const std::string match_query = sanitize_fts_query(query);
    if (match_query.empty() || limit == 0) {
        return {};
    }

    Statement stmt(
        storage.handle(),
        "SELECT m.memory_id, m.node_id, m.memory_type, m.title, m.body, m.created_at, "
        "bm25(fts_memories, 2.0, 1.0) AS score "
        "FROM fts_memories "
        "JOIN memories m ON m.memory_id = fts_memories.rowid "
        "WHERE fts_memories MATCH ? "
        "ORDER BY score, m.created_at DESC "
        "LIMIT ?;"
    );
    bind_text(stmt.get(), 1, match_query);
    bind_int64(stmt.get(), 2, limit);

    std::vector<MemoryIncidentMatch> matches;
    while (stmt.step()) {
        MemoryIncidentMatch match;
        match.memory_id = sqlite3_column_int64(stmt.get(), 0);
        match.node_id = sqlite3_column_int64(stmt.get(), 1);
        match.memory_type = column_text(stmt.get(), 2);
        match.title = column_text(stmt.get(), 3);
        match.body = column_text(stmt.get(), 4);
        match.created_at = column_text(stmt.get(), 5);
        match.score = sqlite3_column_double(stmt.get(), 6);
        matches.push_back(std::move(match));
    }

    // Attach provenance: the paths/symbols each memory affects. A resolved edge
    // points at a node (use its title); an unresolved one keeps the raw to_ref.
    Statement affects_stmt(
        storage.handle(),
        "SELECT COALESCE(n.title, e.to_ref) "
        "FROM edges e LEFT JOIN nodes n ON n.node_id = e.to_node "
        "WHERE e.from_node = ? AND e.kind = 'affects' "
        "AND COALESCE(n.title, e.to_ref) IS NOT NULL "
        "ORDER BY 1;"
    );
    for (MemoryIncidentMatch& match : matches) {
        affects_stmt.reset();
        bind_int64(affects_stmt.get(), 1, match.node_id);
        while (affects_stmt.step()) {
            match.affects.push_back(column_text(affects_stmt.get(), 0));
        }
    }
    return matches;
}

ReadSymbolResult read_symbol_verified(
    Storage& storage,
    const FrontendRegistry& registry,
    const IndexOptions& options,
    std::string_view query
) {
    StoredSymbol stored;
    if (!lookup_symbol(storage, query, stored)) {
        return ReadSymbolResult{
            ReadStatus::NotFound,
            {},
            {},
            "symbol not found",
        };
    }

    const std::filesystem::path repo_root = std::filesystem::weakly_canonical(options.repo_root);
    const std::filesystem::path abs_path = repo_root / stored.match.path;
    bool refreshed = false;

    if (!std::filesystem::is_regular_file(abs_path)) {
        refreshed = true;
    } else {
        const std::string source = read_file_bytes(abs_path);
        refreshed = xxh64_hex(source) != stored.file_hash;
        if (!refreshed) {
            return ReadSymbolResult{
                ReadStatus::Ok,
                stored.match,
                span_text(source, stored.match),
                {},
            };
        }
    }

    if (refreshed) {
        (void)scan_repository(storage, registry, ScanOptions{repo_root, options.repo_id});
        (void)index_repository(storage, registry, options);
    }

    StoredSymbol current;
    if (!lookup_symbol_in_file(storage, stored.match.path, stored.match.qualified_name, current)) {
        return ReadSymbolResult{
            ReadStatus::Gone,
            stored.match,
            {},
            "symbol no longer present",
        };
    }

    const std::filesystem::path current_path = repo_root / current.match.path;
    const std::string current_source = read_file_bytes(current_path);
    return ReadSymbolResult{
        ReadStatus::ReResolved,
        current.match,
        span_text(current_source, current.match),
        {},
    };
}

FileRangeResult read_file_range(
    const std::filesystem::path& repo_root,
    std::string_view path,
    uint32_t start_line,
    uint32_t end_line
) {
    if (start_line == 0 || end_line < start_line) {
        throw std::invalid_argument("line range must be 1-based and end must be >= start");
    }

    const std::filesystem::path abs_path = std::filesystem::weakly_canonical(repo_root) / std::string(path);
    const std::string source = read_file_bytes(abs_path);
    const std::vector<uint32_t> offsets = build_line_offsets(source);
    if (start_line > offsets.size()) {
        throw std::out_of_range("start line is outside file");
    }

    const size_t start_index = static_cast<size_t>(start_line - 1U);
    const size_t end_index = static_cast<size_t>(end_line);
    const size_t start_byte = offsets[start_index];
    const size_t end_byte = end_index < offsets.size() ? offsets[end_index] : source.size();

    return FileRangeResult{
        std::string(path),
        start_line,
        end_line,
        xxh64_hex(source),
        source.substr(start_byte, end_byte - start_byte),
    };
}

}  // namespace codegraph
