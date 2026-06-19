#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "storage.h"

namespace codegraph {

struct CorrectionInput {
    std::string title;
    std::string reason;
    std::vector<std::string> prefer_paths;
    std::vector<std::string> avoid_paths;
    std::vector<std::string> affects;
};

struct DecisionInput {
    std::string title;
    std::string body;
    std::vector<std::string> affects;
};

struct MaterializeResult {
    uint32_t ops_applied = 0;
    uint32_t edges_resolved = 0;
};

std::string ensure_device_id(const std::filesystem::path& codegraph_dir);
std::string append_correction_op(
    const std::filesystem::path& codegraph_dir,
    const CorrectionInput& input
);
std::string append_decision_op(
    const std::filesystem::path& codegraph_dir,
    const DecisionInput& input
);

MaterializeResult materialize(
    Storage& storage,
    const std::filesystem::path& codegraph_dir
);

}  // namespace codegraph
