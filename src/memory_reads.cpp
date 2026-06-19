#include "memory_reads.h"

#include <regex>
#include <string>
#include <unordered_map>

#include <sqlite3.h>

#include "resolver.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

std::string regex_escape(char ch) {
    static constexpr std::string_view kSpecial = R"(\.^$|()[]{}+?)";
    if (kSpecial.find(ch) != std::string_view::npos) {
        return std::string("\\") + ch;
    }
    return std::string(1, ch);
}

std::regex glob_to_regex(std::string_view pattern) {
    std::string regex = "^";
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*') {
            if (i + 1U < pattern.size() && pattern[i + 1U] == '*') {
                regex += ".*";
                ++i;
            } else {
                regex += "[^/]*";
            }
        } else {
            regex += regex_escape(ch);
        }
    }
    regex += "$";
    return std::regex(regex);
}

bool glob_matches(std::string_view pattern, std::string_view path) {
    return std::regex_match(
        std::string(path),
        glob_to_regex(pattern)
    );
}

MemoryView memory_from_statement(sqlite3_stmt* stmt, int provenance_column) {
    const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const auto* body = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const auto* created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    const auto* provenance = reinterpret_cast<const char*>(sqlite3_column_text(stmt, provenance_column));

    MemoryView memory;
    memory.memory_id = sqlite3_column_int64(stmt, 0);
    memory.node_id = sqlite3_column_int64(stmt, 1);
    memory.memory_type = type == nullptr ? "" : type;
    memory.title = title == nullptr ? "" : title;
    memory.body = body == nullptr ? "" : body;
    memory.created_at = created_at == nullptr ? "" : created_at;
    memory.provenance = provenance == nullptr ? "" : provenance;
    return memory;
}

void merge_memory(std::vector<MemoryView>& memories, MemoryView memory) {
    for (MemoryView& existing : memories) {
        if (existing.memory_id == memory.memory_id) {
            existing.path_rules.insert(
                existing.path_rules.end(),
                memory.path_rules.begin(),
                memory.path_rules.end()
            );
            if (existing.provenance.find(memory.provenance) == std::string::npos) {
                existing.provenance += "; " + memory.provenance;
            }
            return;
        }
    }
    memories.push_back(std::move(memory));
}

void add_memory(MemoryReadResult& result, MemoryView memory) {
    if (memory.memory_type == "correction") {
        merge_memory(result.corrections, std::move(memory));
    } else if (memory.memory_type == "arch_decision") {
        merge_memory(result.decisions, std::move(memory));
    }
}

void add_direct_affects(Storage& storage, int64_t target_node, MemoryReadResult& result) {
    if (target_node < 0) {
        return;
    }

    Statement stmt(
        storage.handle(),
        "SELECT DISTINCT m.memory_id, m.node_id, m.memory_type, m.title, m.body, m.created_at, "
        "COALESCE(e.to_ref, '') "
        "FROM edges e "
        "JOIN memories m ON m.node_id = e.from_node "
        "WHERE e.kind = 'affects' AND e.resolved = 1 AND e.to_node = ? "
        "ORDER BY m.created_at, m.memory_id;"
    );
    bind_int64(stmt.get(), 1, target_node);

    while (stmt.step()) {
        MemoryView memory = memory_from_statement(stmt.get(), 6);
        if (memory.provenance.empty()) {
            memory.provenance = "direct affects edge";
        } else {
            memory.provenance = "affects " + memory.provenance;
        }
        add_memory(result, std::move(memory));
    }
}

void add_matching_path_rules(Storage& storage, std::string_view path, MemoryReadResult& result) {
    Statement stmt(
        storage.handle(),
        "SELECT m.memory_id, m.node_id, m.memory_type, m.title, m.body, m.created_at, "
        "pr.rule_kind, pr.pattern, COALESCE(pr.reason, '') "
        "FROM path_rules pr "
        "JOIN memories m ON m.node_id = pr.node_id "
        "ORDER BY m.created_at, m.memory_id, pr.rule_id;"
    );

    while (stmt.step()) {
        const auto* rule_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        const auto* pattern = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        const auto* reason = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        const std::string pattern_text = pattern == nullptr ? "" : pattern;
        if (!glob_matches(pattern_text, path)) {
            continue;
        }

        MemoryView memory = memory_from_statement(stmt.get(), 8);
        memory.provenance = "path rule " +
                            std::string(rule_kind == nullptr ? "" : rule_kind) +
                            " " + pattern_text;
        memory.path_rules.push_back(PathRuleView{
            rule_kind == nullptr ? "" : rule_kind,
            pattern_text,
            reason == nullptr ? "" : reason,
        });
        add_memory(result, std::move(memory));
    }
}

std::string file_part_for_target(std::string_view target) {
    const size_t separator = target.find("::");
    if (separator == std::string_view::npos) {
        return std::string(target);
    }
    return std::string(target.substr(0, separator));
}

}  // namespace

MemoryReadResult memory_for_target(Storage& storage, std::string_view target) {
    MemoryReadResult result;
    result.target = std::string(target);

    add_direct_affects(storage, resolve_reference(storage, target), result);
    add_matching_path_rules(storage, file_part_for_target(target), result);
    return result;
}

}  // namespace codegraph
