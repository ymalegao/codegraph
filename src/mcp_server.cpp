#include "mcp_server.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "file_util.h"
#include "graph_store.h"
#include "hash_util.h"
#include "materializer.h"
#include "memory_reads.h"
#include "read_tools.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

using json = nlohmann::json;

struct McpContext {
    Storage& storage;
    FrontendRegistry& registry;
    IndexOptions options;
    std::filesystem::path codegraph_dir;
    GraphIndex graph;
    int64_t data_version = 0;
    std::ofstream log;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    json input_schema;
    std::function<json(McpContext&, const json&)> handler;
};

int64_t sqlite_data_version(Storage& storage);

std::string required_string(const json& args, std::string_view key) {
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string()) {
        throw std::runtime_error("missing string argument: " + std::string(key));
    }
    return it->get<std::string>();
}

uint32_t optional_u32(const json& args, std::string_view key, uint32_t fallback) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_number_unsigned()) {
        throw std::runtime_error("argument must be an unsigned integer: " + std::string(key));
    }
    return it->get<uint32_t>();
}

bool optional_bool(const json& args, std::string_view key, bool fallback) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        throw std::runtime_error("argument must be boolean: " + std::string(key));
    }
    return it->get<bool>();
}

std::vector<std::string> optional_string_array(const json& args, std::string_view key) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        return {};
    }
    if (!it->is_array()) {
        throw std::runtime_error("argument must be an array: " + std::string(key));
    }

    std::vector<std::string> values;
    for (const json& value : *it) {
        if (!value.is_string()) {
            throw std::runtime_error("array argument contains non-string: " + std::string(key));
        }
        values.push_back(value.get<std::string>());
    }
    return values;
}

std::string location(std::string_view path, uint32_t start_line, uint32_t end_line) {
    return std::string(path) + ":" + std::to_string(start_line) + "-" + std::to_string(end_line);
}

json memory_view_json(const MemoryView& memory) {
    json rules = json::array();
    for (const PathRuleView& rule : memory.path_rules) {
        rules.push_back({
            {"rule_kind", rule.rule_kind},
            {"pattern", rule.pattern},
            {"reason", rule.reason},
        });
    }

    return {
        {"memory_id", memory.memory_id},
        {"node_id", memory.node_id},
        {"memory_type", memory.memory_type},
        {"title", memory.title},
        {"body", memory.body},
        {"created_at", memory.created_at},
        {"provenance", memory.provenance},
        {"path_rules", rules},
    };
}

json memory_result_json(const MemoryReadResult& memory) {
    json corrections = json::array();
    for (const MemoryView& item : memory.corrections) {
        corrections.push_back(memory_view_json(item));
    }

    json decisions = json::array();
    for (const MemoryView& item : memory.decisions) {
        decisions.push_back(memory_view_json(item));
    }

    return {
        {"target", memory.target},
        {"corrections", corrections},
        {"decisions", decisions},
    };
}

json symbol_json(const GraphSymbolView& symbol) {
    return {
        {"node_id", to_u32(symbol.node_id)},
        {"qualified_name", symbol.qualified_name},
        {"name", symbol.name},
        {"kind", symbol_kind_text(symbol.kind)},
        {"file", symbol.path},
        {"path", symbol.path},
        {"start_line", symbol.span.start_line},
        {"end_line", symbol.span.end_line},
        {"start_byte", symbol.span.start_byte},
        {"end_byte", symbol.span.end_byte},
        {"location", location(symbol.path, symbol.span.start_line, symbol.span.end_line)},
        {"signature", symbol.signature},
    };
}

std::vector<NodeId> find_symbol_nodes(const GraphIndex& graph, std::string_view query) {
    return graph_symbols_by_name_hash(graph, query);
}

GraphSymbolView require_one_symbol(const GraphIndex& graph, std::string_view query) {
    const std::vector<NodeId> nodes = find_symbol_nodes(graph, query);
    if (nodes.empty()) {
        throw std::runtime_error("symbol not found: " + std::string(query));
    }
    const auto symbol = graph_symbol(graph, nodes.front());
    if (!symbol.has_value()) {
        throw std::runtime_error("symbol payload is missing: " + std::string(query));
    }
    return *symbol;
}

std::string span_text(std::string_view source, SourceSpan span) {
    if (span.start_byte > span.end_byte || span.end_byte > source.size()) {
        throw std::runtime_error("symbol span is outside current file bytes");
    }
    return std::string(source.substr(span.start_byte, span.end_byte - span.start_byte));
}

std::optional<SymbolInfo> reparse_symbol(
    const FrontendRegistry& registry,
    const std::filesystem::path& repo_root,
    std::string_view path,
    std::string_view qualified_name
) {
    const std::filesystem::path abs_path = repo_root / std::string(path);
    const std::string ext = abs_path.extension().string();
    const LanguageFrontend* frontend = registry.for_extension(ext);
    if (frontend == nullptr) {
        throw std::runtime_error("no frontend for file extension: " + ext);
    }

    const std::string source = read_file_bytes(abs_path);
    const ExtractedFile extracted = frontend->extract(source);
    for (const SymbolInfo& symbol : extracted.symbols) {
        if (symbol.qualified_name == qualified_name) {
            return symbol;
        }
    }
    return std::nullopt;
}

json read_graph_symbol(McpContext& ctx, const GraphSymbolView& symbol) {
    const std::filesystem::path abs_path = ctx.options.repo_root / std::string(symbol.path);
    if (!std::filesystem::is_regular_file(abs_path)) {
        return {
            {"hash_status", read_status_name(ReadStatus::Gone)},
            {"qualified_name", symbol.qualified_name},
            {"path", symbol.path},
            {"message", "symbol file no longer present; try find_symbol"},
        };
    }

    const std::string source = read_file_bytes(abs_path);
    if (xxh64_hex(source) == symbol.file_content_hash) {
        json result = symbol_json(symbol);
        result["hash_status"] = read_status_name(ReadStatus::Ok);
        result["body"] = span_text(source, symbol.span);
        return result;
    }

    const std::optional<SymbolInfo> current =
        reparse_symbol(ctx.registry, ctx.options.repo_root, symbol.path, symbol.qualified_name);
    if (!current.has_value()) {
        return {
            {"hash_status", read_status_name(ReadStatus::Gone)},
            {"qualified_name", symbol.qualified_name},
            {"path", symbol.path},
            {"message", "symbol no longer present; try find_symbol"},
        };
    }

    SourceSpan span{
        current->start_line,
        current->end_line,
        current->start_byte,
        current->end_byte,
        0,
        0,
    };
    json result = {
        {"node_id", to_u32(symbol.node_id)},
        {"qualified_name", current->qualified_name},
        {"name", current->name},
        {"kind", symbol_kind_text(symbol_kind_from_string(current->kind))},
        {"file", symbol.path},
        {"path", symbol.path},
        {"start_line", current->start_line},
        {"end_line", current->end_line},
        {"start_byte", current->start_byte},
        {"end_byte", current->end_byte},
        {"location", location(symbol.path, current->start_line, current->end_line)},
        {"signature", current->signature},
        {"hash_status", read_status_name(ReadStatus::ReResolved)},
        {"body", span_text(source, span)},
    };
    return result;
}

MemoryReadResult graph_memory_for_target(
    McpContext& ctx,
    std::string_view target,
    NodeId node,
    std::string_view path_for_rules
) {
    return memory_for_graph_nodes(
        ctx.storage,
        target,
        graph_memory_for_node(ctx.graph, node),
        path_for_rules
    );
}

json search_symbol_json(McpContext& ctx, const json& args) {
    const std::string query = required_string(args, "query");
    const uint32_t limit = optional_u32(args, "limit", 20);
    std::optional<std::string_view> kind;
    std::string kind_storage;
    if (const auto it = args.find("kind"); it != args.end() && !it->is_null()) {
        if (!it->is_string()) {
            throw std::runtime_error("kind must be a string");
        }
        kind_storage = it->get<std::string>();
        kind = std::string_view(kind_storage);
    }

    json matches = json::array();
    for (const SymbolSearchMatch& match : search_symbols(ctx.storage, query, kind, limit)) {
        matches.push_back({
            {"symbol_id", match.symbol_id},
            {"qualified_name", match.qualified_name},
            {"file", match.path},
            {"path", match.path},
            {"start_line", match.start_line},
            {"end_line", match.end_line},
            {"kind", match.kind},
            {"signature", match.signature},
            {"score", match.score},
            {"location", location(match.path, match.start_line, match.end_line)},
        });
    }
    return matches;
}

json file_range_json(McpContext& ctx, const json& args) {
    const std::string path = required_string(args, "path");
    const uint32_t start = optional_u32(args, "start_line", 1);
    const uint32_t end = optional_u32(args, "end_line", 1);
    if (start == 0 || end == 0) {
        throw std::runtime_error("start_line and end_line must be 1-based");
    }
    const FileRangeResult range = read_file_range(ctx.options.repo_root, path, start, end);
    return {
        {"path", range.path},
        {"start_line", range.start_line},
        {"end_line", range.end_line},
        {"current_hash", range.content_hash},
        {"text", range.text},
    };
}

json find_symbol_json(McpContext& ctx, const json& args) {
    const std::string name = required_string(args, "name");
    const uint32_t limit = optional_u32(args, "limit", 20);

    json matches = json::array();
    const std::vector<NodeId> nodes = find_symbol_nodes(ctx.graph, name);
    for (NodeId node : nodes) {
        if (matches.size() >= limit) {
            break;
        }
        const auto symbol = graph_symbol(ctx.graph, node);
        if (symbol.has_value()) {
            matches.push_back(symbol_json(*symbol));
        }
    }
    return matches;
}

json read_symbol_json(McpContext& ctx, const json& args) {
    const std::string query = required_string(args, "query");
    const bool include_memory = optional_bool(args, "include_memory", true);
    const GraphSymbolView symbol = require_one_symbol(ctx.graph, query);
    json result = read_graph_symbol(ctx, symbol);
    if (include_memory) {
        result["memory"] = memory_result_json(
            graph_memory_for_target(ctx, query, symbol.node_id, symbol.path)
        );
    }
    return result;
}

std::optional<GraphSymbolView> enclosing_symbol(
    const GraphIndex& graph,
    NodeId file_node,
    uint32_t line
) {
    std::vector<NodeId> stack = csr_neighbors(graph.graph.forward, file_node, EdgeKind::Contains);
    std::optional<GraphSymbolView> best;
    while (!stack.empty()) {
        const NodeId node = stack.back();
        stack.pop_back();

        for (NodeId child : csr_neighbors(graph.graph.forward, node, EdgeKind::Contains)) {
            stack.push_back(child);
        }

        const auto symbol = graph_symbol(graph, node);
        if (!symbol.has_value()) {
            continue;
        }
        if (symbol->span.start_line > line || symbol->span.end_line < line) {
            continue;
        }
        if (!best.has_value() ||
            (symbol->span.end_byte - symbol->span.start_byte) <
                (best->span.end_byte - best->span.start_byte)) {
            best = symbol;
        }
    }
    return best;
}

json read_enclosing_symbol_json(McpContext& ctx, const json& args) {
    const std::string path = required_string(args, "path");
    const uint32_t line = optional_u32(args, "line", 0);
    if (line == 0) {
        throw std::runtime_error("line must be 1-based");
    }

    const auto file_node = graph_file_node_by_path(ctx.graph, path);
    if (!file_node.has_value()) {
        throw std::runtime_error("file not found in graph: " + path);
    }
    const auto symbol = enclosing_symbol(ctx.graph, *file_node, line);
    if (!symbol.has_value()) {
        throw std::runtime_error("no enclosing symbol found");
    }
    return read_graph_symbol(ctx, *symbol);
}

json get_memory_for_file_json(McpContext& ctx, const json& args) {
    const std::string path = required_string(args, "path");
    const auto node = graph_file_node_by_path(ctx.graph, path);
    if (!node.has_value()) {
        throw std::runtime_error("file not found in graph: " + path);
    }
    return memory_result_json(graph_memory_for_target(ctx, path, *node, path));
}

json get_memory_for_symbol_json(McpContext& ctx, const json& args) {
    const std::string query = required_string(args, "query");
    const GraphSymbolView symbol = require_one_symbol(ctx.graph, query);
    return memory_result_json(graph_memory_for_target(ctx, query, symbol.node_id, symbol.path));
}

int64_t node_id_for_memory_op(Storage& storage, std::string_view op_id) {
    Statement stmt(storage.handle(), "SELECT node_id FROM nodes WHERE stable_id = ?;");
    const std::string stable_id = "memory:" + std::string(op_id);
    bind_text(stmt.get(), 1, stable_id);
    stmt.expect_row("select materialized memory node");
    return sqlite3_column_int64(stmt.get(), 0);
}


json write_memory(McpContext& ctx, const json& args, const MemoryKind& kind) {
    const std::string title = kind.title_required
                                  ? required_string(args, "title")
                                  : args.value("title", std::string(kind.default_title));
    const std::string body = required_string(args, kind.body_field);
    const std::vector<std::string> affects = optional_string_array(args, "affects");
    std::vector<std::string> prefer_paths;
    std::vector<std::string> avoid_paths;
    if (kind.supports_path_rules) {
        prefer_paths = optional_string_array(args, "prefer_paths");
        avoid_paths = optional_string_array(args, "avoid_paths");
    }

    switch (kind.affects_rule) {
        case AffectsRule::Required:
            if (affects.empty()) {
                throw std::runtime_error(
                    std::string(kind.tool_name) + " requires at least one affects entry"
                );
            }
            break;
        case AffectsRule::RequiredUnlessPathRules:
            if (affects.empty() && prefer_paths.empty() && avoid_paths.empty()) {
                throw std::runtime_error(
                    std::string(kind.tool_name) + " needs affects, prefer_paths, or avoid_paths"
                );
            }
            break;
        case AffectsRule::Optional:
            break;
    }

    json payload{
        {"title", title},
        {std::string(kind.body_field), body},
        {"affects", affects},
    };
    if (kind.supports_path_rules) {
        payload["prefer_paths"] = prefer_paths;
        payload["avoid_paths"] = avoid_paths;
    }

    std::filesystem::create_directories(ctx.codegraph_dir);
    const std::string op_id = append_memory_op(ctx.codegraph_dir, kind, payload);
    const MaterializeResult materialized = materialize(ctx.storage, ctx.codegraph_dir);
    const int64_t node_id = node_id_for_memory_op(ctx.storage, op_id);
    ctx.graph = build_graph_index(ctx.storage);
    ctx.data_version = sqlite_data_version(ctx.storage);
    return {
        {"op_id", op_id},
        {"node_id", node_id},
        {"ops_applied", materialized.ops_applied},
        {"edges_resolved", materialized.edges_resolved},
    };
}

json schema_object(std::initializer_list<std::pair<std::string, json>> properties) {
    json props = json::object();
    for (const auto& [name, schema] : properties) {
        props[name] = schema;
    }
    return {{"type", "object"}, {"properties", props}};
}

json string_schema(std::string description = {}) {
    json schema = {{"type", "string"}};
    if (!description.empty()) {
        schema["description"] = std::move(description);
    }
    return schema;
}

json uint_schema(uint32_t fallback) {
    return {{"type", "integer"}, {"minimum", 0}, {"default", fallback}};
}


json resume_from_handoff_schema() {
    return schema_object({});
}


json memory_tool_schema(const MemoryKind& kind) {
    json properties = json::object();
    properties["title"] = string_schema();
    properties[std::string(kind.body_field)] = string_schema();
    properties["affects"] = {{"type", "array"}, {"items", string_schema()}};
    if (kind.supports_path_rules) {
        properties["prefer_paths"] = {{"type", "array"}, {"items", string_schema()}};
        properties["avoid_paths"] = {{"type", "array"}, {"items", string_schema()}};
    }
    return {{"type", "object"}, {"properties", properties}};
}



json resume_from_handoff(McpContext& ctx, const json&) {
  
    
    ResumeContext context  = build_resume_context(ctx.storage, ctx.graph);

    json affected = json::array();
    for (const AffectedNodeView& node : context.affected_nodes) {
        affected.push_back({
            {"node_id", to_u32(node.node_id)},
            {"kind", node.kind},
            {"title", node.title},
            {"path", node.path},
            {"qualified_name", node.qualified_name},
        });
    }

    return {
        {"handoff", context.handoff_body},
        {"affected_nodes", affected},
        {"unresolved_affects", context.unresolved_affects},
        {"found", context.found},
    };
}


std::vector<ToolDefinition> tool_registry() {
    std::vector<ToolDefinition> tools{
        ToolDefinition{
            "find_symbol",
            "Exact symbol lookup by known name or qualified name using the in-memory graph.",
            schema_object({{"name", string_schema()}, {"limit", uint_schema(20)}}),
            find_symbol_json,
        },
        ToolDefinition{
            "read_symbol",
            "Read a symbol body with verify-before-trust and optional memory.",
            schema_object({
                {"query", string_schema()},
                {"include_memory", {{"type", "boolean"}, {"default", true}}},
            }),
            read_symbol_json,
        },
        ToolDefinition{
            "read_enclosing_symbol",
            "Read the smallest symbol enclosing a file line.",
            schema_object({{"path", string_schema()}, {"line", uint_schema(0)}}),
            read_enclosing_symbol_json,
        },
        ToolDefinition{
            "read_file_range",
            "Read an exact line range from disk.",
            schema_object({
                {"path", string_schema()},
                {"start_line", uint_schema(1)},
                {"end_line", uint_schema(1)},
            }),
            file_range_json,
        },
        ToolDefinition{
            "get_memory_for_file",
            "Get graph-attached and path-rule memory for a file.",
            schema_object({{"path", string_schema()}}),
            get_memory_for_file_json,
        },
        ToolDefinition{
            "get_memory_for_symbol",
            "Get graph-attached and path-rule memory for a symbol.",
            schema_object({{"query", string_schema()}}),
            get_memory_for_symbol_json,
        },
        ToolDefinition{
            "search_symbol",
            "Fuzzy keyword symbol search using SQLite FTS5.",
            schema_object({
                {"query", string_schema()},
                {"kind", string_schema()},
                {"limit", uint_schema(20)},
            }),
            search_symbol_json,
        },
    };
    for (const MemoryKind& memory_kind : kMemoryKinds) {
        const MemoryKind* kind = &memory_kind;
        tools.push_back(ToolDefinition{
            std::string(kind->tool_name),
            std::string(kind->tool_description),
            memory_tool_schema(*kind),
            [kind](McpContext& ctx, const json& args) {
                return write_memory(ctx, args, *kind);
            },
        });
    }
    tools.push_back(
        ToolDefinition{
            "resume_from_handoff",
            "Resume the session from the latest handoff.",
            resume_from_handoff_schema(),
            resume_from_handoff,
        }
    );
    return tools;
}

json tool_response(json id, const json& result, bool is_error = false) {
    json body = {
        {"content", json::array({{{"type", "text"}, {"text", result.dump()}}})},
    };
    if (is_error) {
        body["isError"] = true;
    }
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(body)}};
}

json rpc_result(json id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}

json rpc_error(json id, int code, std::string message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"error", {{"code", code}, {"message", std::move(message)}}},
    };
}

json tools_list_result(const std::vector<ToolDefinition>& tools) {
    json listed = json::array();
    for (const ToolDefinition& tool : tools) {
        listed.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", tool.input_schema},
        });
    }
    return {{"tools", listed}};
}

void log_line(McpContext& ctx, std::string_view message) {
    std::cerr << message << "\n";
    if (ctx.log.is_open()) {
        ctx.log << message << "\n";
        ctx.log.flush();
    }
}

int64_t sqlite_data_version(Storage& storage) {
    Statement stmt(storage.handle(), "PRAGMA data_version;");
    stmt.expect_row("read sqlite data_version");
    return sqlite3_column_int64(stmt.get(), 0);
}

bool read_tool_name(std::string_view name) {
    return name == "find_symbol" ||
           name == "read_symbol" ||
           name == "read_enclosing_symbol" ||
           name == "read_file_range" ||
           name == "get_memory_for_file" ||
           name == "get_memory_for_symbol" ||
           name == "search_symbol" ||
           memory_kind_by_tool_name(name) != nullptr ||
           name == "resume_from_handoff";

}

void rebuild_graph_if_external_change(McpContext& ctx) {
    const int64_t current = sqlite_data_version(ctx.storage);
    if (current != ctx.data_version) {
        ctx.graph = build_graph_index(ctx.storage);
        ctx.data_version = current;
        log_line(ctx, "rebuilt graph after external sqlite commit");
    }
}

std::optional<json> handle_request(
    McpContext& ctx,
    const std::vector<ToolDefinition>& tools,
    const json& request
) {
    if (!request.is_object()) {
        return rpc_error(nullptr, -32600, "invalid request");
    }

    const std::string method = request.value("method", "");
    const bool has_id = request.contains("id");
    json id = has_id ? request["id"] : json(nullptr);

    if (method == "notifications/initialized") {
        return std::nullopt;
    }

    if (method == "initialize") {
        const std::string protocol = request.value("params", json::object())
                                         .value("protocolVersion", "2025-06-18");
        return rpc_result(id, {
            {"protocolVersion", protocol},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "codegraph"}, {"version", "0.1.0"}}},
        });
    }

    if (method == "ping") {
        return rpc_result(id, json::object());
    }

    if (method == "tools/list") {
        return rpc_result(id, tools_list_result(tools));
    }

    if (method == "tools/call") {
        const json params = request.value("params", json::object());
        const std::string name = params.value("name", "");
        const json args = params.value("arguments", json::object());
        const auto it = std::find_if(tools.begin(), tools.end(), [&](const ToolDefinition& tool) {
            return tool.name == name;
        });
        if (it == tools.end()) {
            return tool_response(id, {{"error", "unknown tool: " + name}}, true);
        }

        try {
            if (read_tool_name(name)) {
                rebuild_graph_if_external_change(ctx);
            }
            return tool_response(id, it->handler(ctx, args), false);
        } catch (const std::exception& ex) {
            log_line(ctx, std::string("tool failed: ") + name + ": " + ex.what());
            return tool_response(id, {{"error", ex.what()}}, true);
        }
    }

    if (has_id) {
        return rpc_error(id, -32601, "method not found: " + method);
    }
    return std::nullopt;
}





}  // namespace

int run_mcp_server(
    Storage& storage,
    FrontendRegistry& registry,
    const IndexOptions& options,
    const std::filesystem::path& codegraph_dir
) {
    std::filesystem::create_directories(codegraph_dir / "logs");
    McpContext ctx{
        storage,
        registry,
        options,
        codegraph_dir,
        build_graph_index(storage),
        sqlite_data_version(storage),
        std::ofstream(codegraph_dir / "logs" / "mcp.log", std::ios::app),
    };
    const std::vector<ToolDefinition> tools = tool_registry();
    log_line(ctx, "codegraph mcp started");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        std::optional<json> response;
        try {
            response = handle_request(ctx, tools, json::parse(line));
        } catch (const json::parse_error& ex) {
            response = rpc_error(nullptr, -32700, ex.what());
        } catch (const std::exception& ex) {
            response = rpc_error(nullptr, -32603, ex.what());
        }

        if (response.has_value()) {
            std::cout << response->dump() << "\n";
            std::cout.flush();
        }
    }
    log_line(ctx, "codegraph mcp stopped");
    return 0;
}



}  // namespace codegraph
