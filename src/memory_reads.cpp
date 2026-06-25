#include "memory_reads.h"

#include <string>
#include <vector>

#include <sqlite3.h>

#include "core.h"
#include "resolver.h"
#include "sqlite_util.h"
#include "graph_store.h"

namespace codegraph {
namespace {

bool glob_match_from(std::string_view pattern, size_t pi, std::string_view path, size_t si) {
    while (pi < pattern.size()) {
        if (pattern[pi] == '*') {
            const bool globstar = pi + 1U < pattern.size() && pattern[pi + 1U] == '*';
            const size_t next_pi = pi + (globstar ? 2U : 1U);
            for (size_t next_si = si; next_si <= path.size(); ++next_si) {
                if (!globstar && path.substr(si, next_si - si).find('/') != std::string_view::npos) {
                    break;
                }
                if (glob_match_from(pattern, next_pi, path, next_si)) {
                    return true;
                }
            }
            return false;
        }

        if (si >= path.size() || pattern[pi] != path[si]) {
            return false;
        }
        ++pi;
        ++si;
    }
    return si == path.size();
}

bool glob_matches(std::string_view pattern, std::string_view path) {
    return glob_match_from(pattern, 0, path, 0);
}

MemoryView memory_from_statement(sqlite3_stmt* stmt, int provenance_column) {
    MemoryView memory;
    memory.memory_id = sqlite3_column_int64(stmt, 0);
    memory.node_id = sqlite3_column_int64(stmt, 1);
    memory.memory_type = column_text(stmt, 2);
    memory.title = column_text(stmt, 3);
    memory.body = column_text(stmt, 4);
    memory.created_at = column_text(stmt, 5);
    memory.provenance = column_text(stmt, provenance_column);
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
    const MemoryType memory_type = memory_type_from_string(memory.memory_type);
    if (memory_type == MemoryType::Correction) {
        merge_memory(result.corrections, std::move(memory));
    } else if (memory_type == MemoryType::ArchDecision) {
        merge_memory(result.decisions, std::move(memory));
    } else if (memory_type == MemoryType::Handoff) {
        // Handoffs affecting a file/symbol are surfaced too — otherwise a file whose
        // only attached memory is a handoff looks like it has none. Bodies are
        // snippeted at the JSON layer to avoid dumping full handoff text per target.
        merge_memory(result.handoffs, std::move(memory));
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

void add_graph_memory_nodes(
    Storage& storage,
    const std::vector<NodeId>& memory_nodes,
    MemoryReadResult& result
) {
    Statement stmt(
        storage.handle(),
        "SELECT m.memory_id, m.node_id, m.memory_type, m.title, m.body, m.created_at, "
        "'' "
        "FROM memories m "
        "JOIN nodes n ON n.node_id = m.node_id "
        "WHERE m.node_id = ? AND n.status = ?;"
    );

    for (NodeId node : memory_nodes) {
        stmt.reset();
        bind_int64(stmt.get(), 1, to_u32(node));
        bind_text(stmt.get(), 2, status_text(Status::Active));
        while (stmt.step()) {
            MemoryView memory = memory_from_statement(stmt.get(), 6);
            memory.provenance = "direct affects edge";
            add_memory(result, std::move(memory));
        }
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
        const std::string rule_kind = column_text(stmt.get(), 6);
        const std::string pattern_text = column_text(stmt.get(), 7);
        const std::string reason = column_text(stmt.get(), 8);
        if (!glob_matches(pattern_text, path)) {
            continue;
        }

        MemoryView memory = memory_from_statement(stmt.get(), 8);
        memory.provenance = "path rule " + rule_kind + " " + pattern_text;
        memory.path_rules.push_back(PathRuleView{
            rule_kind,
            pattern_text,
            reason,
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

MemoryReadResult memory_for_graph_nodes(
    Storage& storage,
    std::string_view target,
    const std::vector<NodeId>& memory_nodes,
    std::string_view path_for_rules
) {
    MemoryReadResult result;
    result.target = std::string(target);

    add_graph_memory_nodes(storage, memory_nodes, result);
    add_matching_path_rules(storage, path_for_rules, result);
    return result;
}


std::vector<MemoryView> latest_handoffs(Storage& storage, uint32_t limit) {
    std::vector<MemoryView> handoffs;
    Statement stmt(
        storage.handle(),
        "SELECT m.memory_id, m.node_id, m.memory_type, m.title, m.body, m.created_at, "
        "       'latest handoff' "
        "FROM memories m "
        "JOIN nodes n ON n.node_id = m.node_id "
        "WHERE m.memory_type = 'handoff' "
        "  AND n.status = 'active' "
        "ORDER BY m.created_at DESC, m.memory_id DESC "
        "LIMIT ?;"
    );
    bind_int64(stmt.get(), 1, static_cast<int64_t>(limit));

    while (stmt.step()) {
        handoffs.push_back(memory_from_statement(stmt.get(), 6));
    }


    return handoffs;
}


ResumeContext build_resume_context(Storage& storage, const GraphIndex& graph) {
   
  ResumeContext context; 
  std::vector<MemoryView> handoffs = latest_handoffs(storage, 1);
  if (handoffs.empty()) {
        context.found = false;
        return context;                       // no handoff yet — caller shows "none"
    }
    const MemoryView& handoff = handoffs.front();
    context.handoff_body = handoff.body;
    const NodeId handoff_node = static_cast<NodeId>(handoff.node_id);

    // resolved affects -> hydrate from the graph (no SQL)
    for (NodeId node : csr_neighbors(graph.graph.forward, handoff_node, EdgeKind::Affects)) {
        const Node& n = graph.graph.nodes[to_u32(node)];
        AffectedNodeView view;
        view.node_id = node;
        view.kind  = std::string(node_kind_text(n.kind));
        view.title = std::string(graph.interner.view(n.title));

        if (n.kind == NodeKind::File) {
            view.path = view.title;           // file node title IS the path
        } else if (const auto sym = graph_symbol(graph, node); sym.has_value()) {
            view.path = sym->path;            // already std::string members
            view.qualified_name = sym->qualified_name;
        }
        context.affected_nodes.push_back(std::move(view));
    }

    // unresolved affects (target deleted/renamed since the handoff) -> SQL by to_ref
    Statement stmt(
        storage.handle(),
        "SELECT to_ref FROM edges "
        "WHERE from_node = ? AND kind = 'affects' AND resolved = 0 AND to_ref IS NOT NULL;"
    );
    bind_int64(stmt.get(), 1, handoff.node_id);
    while (stmt.step()) {
        context.unresolved_affects.push_back(column_text(stmt.get(), 0));
    }

    context.found = true;
    return context;
}

}  // namespace codegraph
