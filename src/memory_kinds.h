#pragma once

#include "core.h"

#include <array>
#include <string_view>

namespace codegraph {

enum class AffectsRule { Optional, Required, RequiredUnlessPathRules };

struct MemoryKind {
    MemoryType type;
    NodeKind node_kind;
    std::string_view text;
    std::string_view op_type;
    std::string_view tool_name;
    std::string_view tool_description;
    std::string_view default_title;
    std::string_view body_field;
    bool title_required;
    bool supports_path_rules;
    AffectsRule affects_rule;
};

inline constexpr std::array<MemoryKind, 3> kMemoryKinds = {{
    {
        MemoryType::Correction,
        NodeKind::Correction,
        "correction",
        "ADD_CORRECTION",
        "record_correction",
        "Append and materialize a correction memory, then rebuild the graph.",
        "Correction",
        "reason",
        false,
        true,
        AffectsRule::RequiredUnlessPathRules,
    },
    {
        MemoryType::ArchDecision,
        NodeKind::ArchDecision,
        "arch_decision",
        "ADD_DECISION",
        "record_decision",
        "Append and materialize an architectural decision, then rebuild the graph.",
        "Decision",
        "body",
        true,
        false,
        AffectsRule::Required,
    },
    {
        MemoryType::Handoff,
        NodeKind::Handoff,
        "handoff",
        "ADD_HANDOFF",
        "write_handoff",
        "Append and materialize a handoff memory, then rebuild the graph.",
        "Handoff",
        "body",
        false,
        false,
        AffectsRule::Optional,
    },
}};

const MemoryKind* memory_kind_by_op_type(std::string_view op_type);
const MemoryKind* memory_kind_by_node_kind(NodeKind kind);
const MemoryKind* memory_kind_by_memory_type(MemoryType type);
const MemoryKind* memory_kind_by_text(std::string_view text);
const MemoryKind* memory_kind_by_tool_name(std::string_view tool_name);

}  // namespace codegraph
