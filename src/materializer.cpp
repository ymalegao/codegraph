#include "materializer.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "file_util.h"
#include "hash_util.h"
#include "resolver.h"
#include "sqlite_util.h"
#include "time_util.h"

namespace codegraph {
namespace {

using json = nlohmann::json;

struct Operation {
    std::string op_id;
    std::string device_id;
    int64_t lamport = 0;
    std::string created_at;
    std::string op_type;
    json payload;
};

std::string trim_trailing_newlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string read_text_file(const std::filesystem::path& path) {
    return read_file_bytes(path);
}

void write_text_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    output << contents;
}

std::string generate_device_id(const std::filesystem::path& codegraph_dir) {
    std::random_device random;
    const std::string seed =
        codegraph_dir.generic_string() + ":" +
        current_utc_timestamp() + ":" +
        std::to_string(static_cast<uint64_t>(random()));
    return "device-" + xxh64_hex(seed);
}

std::filesystem::path ops_dir(const std::filesystem::path& codegraph_dir) {
    return codegraph_dir / "ops";
}

std::filesystem::path op_log_path(
    const std::filesystem::path& codegraph_dir,
    std::string_view device_id
) {
    return ops_dir(codegraph_dir) / (std::string(device_id) + ".jsonl");
}

int64_t max_lamport_for_device(
    const std::filesystem::path& codegraph_dir,
    std::string_view device_id
) {
    const std::filesystem::path path = op_log_path(codegraph_dir, device_id);
    if (!std::filesystem::exists(path)) {
        return 0;
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to read op log: " + path.string());
    }

    std::string last_line;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        last_line = line;
    }
    if (last_line.empty()) {
        return 0;
    }

    const json op = json::parse(last_line);
    if (op.value("device_id", "") != device_id) {
        throw std::runtime_error("device op log contains an op for another device");
    }
    return op.value("lamport", int64_t{0});
}

std::string append_op(
    const std::filesystem::path& codegraph_dir,
    std::string_view op_type,
    const json& payload
) {
    std::filesystem::create_directories(ops_dir(codegraph_dir));
    const std::string device_id = ensure_device_id(codegraph_dir);
    const int64_t lamport = max_lamport_for_device(codegraph_dir, device_id) + 1;
    const std::string op_id = device_id + ":" + std::to_string(lamport);

    const json op{
        {"op_id", op_id},
        {"device_id", device_id},
        {"lamport", lamport},
        {"created_at", current_utc_timestamp()},
        {"op_type", op_type},
        {"payload", payload},
    };

    std::ofstream output(op_log_path(codegraph_dir, device_id), std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("failed to append op log");
    }
    output << op.dump() << "\n";
    return op_id;
}

std::vector<std::string> json_string_array(const json& value, std::string_view field) {
    std::vector<std::string> result;
    const std::string key(field);
    if (!value.contains(key)) {
        return result;
    }
    for (const json& item : value.at(key)) {
        result.push_back(item.get<std::string>());
    }
    return result;
}

Operation parse_operation(const std::string& line) {
    const json value = json::parse(line);
    return Operation{
        value.at("op_id").get<std::string>(),
        value.at("device_id").get<std::string>(),
        value.at("lamport").get<int64_t>(),
        value.at("created_at").get<std::string>(),
        value.at("op_type").get<std::string>(),
        value.at("payload"),
    };
}

std::vector<Operation> read_operations(const std::filesystem::path& codegraph_dir) {
    std::vector<Operation> ops;
    const std::filesystem::path dir = ops_dir(codegraph_dir);
    if (!std::filesystem::exists(dir)) {
        return ops;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
            continue;
        }

        std::ifstream input(entry.path());
        if (!input) {
            throw std::runtime_error("failed to read op log: " + entry.path().string());
        }

        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty()) {
                ops.push_back(parse_operation(line));
            }
        }
    }

    std::sort(ops.begin(), ops.end(), [](const Operation& lhs, const Operation& rhs) {
        if (lhs.lamport != rhs.lamport) {
            return lhs.lamport < rhs.lamport;
        }
        return lhs.device_id < rhs.device_id;
    });
    return ops;
}

std::unordered_set<std::string> load_applied_ops(Storage& storage) {
    Statement stmt(storage.handle(), "SELECT op_id FROM op_index;");
    std::unordered_set<std::string> applied;
    while (stmt.step()) {
        applied.insert(column_text(stmt.get(), 0));
    }
    return applied;
}

int64_t upsert_memory_node(
    Storage& storage,
    std::string_view stable_id,
    std::string_view kind,
    std::string_view title,
    std::string_view created_at
) {
    Statement upsert(
        storage.handle(),
        "INSERT INTO nodes(stable_id, kind, title, created_at, status) "
        "VALUES (?, ?, ?, ?, 'active') "
        "ON CONFLICT(stable_id) DO UPDATE SET "
        "kind = excluded.kind, title = excluded.title, status = 'active';"
    );
    bind_text(upsert.get(), 1, stable_id);
    bind_text(upsert.get(), 2, kind);
    bind_text(upsert.get(), 3, title);
    bind_text(upsert.get(), 4, created_at);
    upsert.expect_done("upsert memory node");

    Statement select(storage.handle(), "SELECT node_id FROM nodes WHERE stable_id = ?;");
    bind_text(select.get(), 1, stable_id);
    select.expect_row("select memory node");
    return sqlite3_column_int64(select.get(), 0);
}

void insert_memory(
    Storage& storage,
    int64_t node_id,
    std::string_view memory_type,
    std::string_view title,
    std::string_view body,
    std::string_view created_at
) {
    Statement stmt(
        storage.handle(),
        "INSERT INTO memories(node_id, memory_type, title, body, created_at) "
        "VALUES (?, ?, ?, ?, ?);"
    );
    bind_int64(stmt.get(), 1, node_id);
    bind_text(stmt.get(), 2, memory_type);
    bind_text(stmt.get(), 3, title);
    bind_text(stmt.get(), 4, body);
    bind_text(stmt.get(), 5, created_at);
    stmt.expect_done("insert memory");
}

void insert_path_rule(
    Storage& storage,
    int64_t node_id,
    std::string_view rule_kind,
    std::string_view pattern,
    std::string_view reason
) {
    Statement stmt(
        storage.handle(),
        "INSERT INTO path_rules(node_id, rule_kind, pattern, reason) "
        "VALUES (?, ?, ?, ?);"
    );
    bind_int64(stmt.get(), 1, node_id);
    bind_text(stmt.get(), 2, rule_kind);
    bind_text(stmt.get(), 3, pattern);
    bind_text(stmt.get(), 4, reason);
    stmt.expect_done("insert path rule");
}

void insert_affects_edge(
    Storage& storage,
    int64_t memory_node,
    std::string_view to_ref
) {
    const int64_t target = resolve_reference(storage, to_ref);
    Statement stmt(
        storage.handle(),
        "INSERT INTO edges(from_node, to_node, to_ref, kind, resolved) "
        "VALUES (?, ?, ?, 'affects', ?);"
    );
    bind_int64(stmt.get(), 1, memory_node);
    if (target >= 0) {
        bind_int64(stmt.get(), 2, target);
    } else {
        check_sqlite(
            sqlite3_bind_null(stmt.get(), 2),
            sqlite3_db_handle(stmt.get()),
            "bind unresolved affects target"
        );
    }
    bind_text(stmt.get(), 3, to_ref);
    bind_int64(stmt.get(), 4, target >= 0 ? 1 : 0);
    stmt.expect_done("insert affects edge");
}

std::string memory_stable_id(const Operation& op) {
    return "memory:" + op.device_id + ":" + std::to_string(op.lamport);
}

void mark_applied(Storage& storage, const Operation& op) {
    Statement stmt(
        storage.handle(),
        "INSERT INTO op_index(op_id, device_id, lamport, op_type, applied_at) "
        "VALUES (?, ?, ?, ?, ?);"
    );
    bind_text(stmt.get(), 1, op.op_id);
    bind_text(stmt.get(), 2, op.device_id);
    bind_int64(stmt.get(), 3, op.lamport);
    bind_text(stmt.get(), 4, op.op_type);
    bind_text(stmt.get(), 5, current_utc_timestamp());
    stmt.expect_done("mark op applied");
}

void apply_correction(Storage& storage, const Operation& op) {
    const std::string title = op.payload.value("title", "Correction");
    const std::string reason = op.payload.value("reason", "");
    const int64_t node_id = upsert_memory_node(
        storage,
        memory_stable_id(op),
        "correction",
        title,
        op.created_at
    );
    insert_memory(storage, node_id, "correction", title, reason, op.created_at);

    for (const std::string& pattern : json_string_array(op.payload, "prefer_paths")) {
        insert_path_rule(storage, node_id, "prefer", pattern, reason);
    }
    for (const std::string& pattern : json_string_array(op.payload, "avoid_paths")) {
        insert_path_rule(storage, node_id, "avoid", pattern, reason);
    }
    for (const std::string& ref : json_string_array(op.payload, "affects")) {
        insert_affects_edge(storage, node_id, ref);
    }
}

void apply_decision(Storage& storage, const Operation& op) {
    const std::string title = op.payload.value("title", "Decision");
    const std::string body = op.payload.value("body", "");
    const int64_t node_id = upsert_memory_node(
        storage,
        memory_stable_id(op),
        "arch_decision",
        title,
        op.created_at
    );
    insert_memory(storage, node_id, "arch_decision", title, body, op.created_at);

    for (const std::string& ref : json_string_array(op.payload, "affects")) {
        insert_affects_edge(storage, node_id, ref);
    }
}

void apply_operation(Storage& storage, const Operation& op) {
    if (op.op_type == "ADD_CORRECTION") {
        apply_correction(storage, op);
        return;
    }
    if (op.op_type == "ADD_DECISION") {
        apply_decision(storage, op);
        return;
    }
    throw std::runtime_error("unknown op type: " + op.op_type);
}

}  // namespace

std::string ensure_device_id(const std::filesystem::path& codegraph_dir) {
    std::filesystem::create_directories(codegraph_dir);
    const std::filesystem::path device_path = codegraph_dir / "device_id";
    if (std::filesystem::exists(device_path)) {
        const std::string existing = trim_trailing_newlines(read_text_file(device_path));
        if (!existing.empty()) {
            return existing;
        }
    }

    const std::string device_id = generate_device_id(codegraph_dir);
    write_text_file(device_path, device_id + "\n");
    return device_id;
}

std::string append_correction_op(
    const std::filesystem::path& codegraph_dir,
    const CorrectionInput& input
) {
    const json payload{
        {"title", input.title.empty() ? "Correction" : input.title},
        {"reason", input.reason},
        {"prefer_paths", input.prefer_paths},
        {"avoid_paths", input.avoid_paths},
        {"affects", input.affects},
    };
    return append_op(codegraph_dir, "ADD_CORRECTION", payload);
}

std::string append_decision_op(
    const std::filesystem::path& codegraph_dir,
    const DecisionInput& input
) {
    const json payload{
        {"title", input.title},
        {"body", input.body},
        {"affects", input.affects},
    };
    return append_op(codegraph_dir, "ADD_DECISION", payload);
}

MaterializeResult materialize(
    Storage& storage,
    const std::filesystem::path& codegraph_dir
) {
    const std::vector<Operation> ops = read_operations(codegraph_dir);
    std::unordered_set<std::string> applied_ops = load_applied_ops(storage);
    MaterializeResult result;

    storage.execute("BEGIN IMMEDIATE;");
    try {
        for (const Operation& op : ops) {
            if (applied_ops.find(op.op_id) != applied_ops.end()) {
                continue;
            }
            apply_operation(storage, op);
            mark_applied(storage, op);
            applied_ops.insert(op.op_id);
            ++result.ops_applied;
        }
        result.edges_resolved = resolver_pass(storage);
        storage.execute("COMMIT;");
    } catch (...) {
        storage.execute("ROLLBACK;");
        throw;
    }

    return result;
}

}  // namespace codegraph
