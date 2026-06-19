#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storage.h"

namespace codegraph {

struct PathRuleView {
    std::string rule_kind;
    std::string pattern;
    std::string reason;
};

struct MemoryView {
    int64_t memory_id = 0;
    int64_t node_id = 0;
    std::string memory_type;
    std::string title;
    std::string body;
    std::string created_at;
    std::string provenance;
    std::vector<PathRuleView> path_rules;
};

struct MemoryReadResult {
    std::string target;
    std::vector<MemoryView> corrections;
    std::vector<MemoryView> decisions;
};

MemoryReadResult memory_for_target(Storage& storage, std::string_view target);

}  // namespace codegraph
