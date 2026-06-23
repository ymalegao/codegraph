#include "memory_kinds.h"

namespace codegraph {

const MemoryKind* memory_kind_by_op_type(std::string_view op_type) {
    for (const MemoryKind& kind : kMemoryKinds) {
        if (kind.op_type == op_type) {
            return &kind;
        }
    }
    return nullptr;
}

const MemoryKind* memory_kind_by_node_kind(NodeKind node_kind) {
    for (const MemoryKind& kind : kMemoryKinds) {
        if (kind.node_kind == node_kind) {
            return &kind;
        }
    }
    return nullptr;
}

const MemoryKind* memory_kind_by_memory_type(MemoryType type) {
    for (const MemoryKind& kind : kMemoryKinds) {
        if (kind.type == type) {
            return &kind;
        }
    }
    return nullptr;
}

const MemoryKind* memory_kind_by_text(std::string_view text) {
    for (const MemoryKind& kind : kMemoryKinds) {
        if (kind.text == text) {
            return &kind;
        }
    }
    return nullptr;
}

const MemoryKind* memory_kind_by_tool_name(std::string_view tool_name) {
    for (const MemoryKind& kind : kMemoryKinds) {
        if (kind.tool_name == tool_name) {
            return &kind;
        }
    }
    return nullptr;
}

}  // namespace codegraph
