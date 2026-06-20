#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "frontend.h"
#include "indexer.h"
#include "materializer.h"
#include "scanner.h"
#include "storage.h"

namespace codegraph {

struct RepoConfig {
    std::string repo_id;
    std::vector<std::string> ignore_patterns;
    size_t max_file_size_mb = 10;
};

struct BootstrapResult {
    RepoConfig config;
    ScanResult scan;
    IndexResult index;
    MaterializeResult materialize;
};

RepoConfig load_or_create_config(const std::filesystem::path& repo_root);

BootstrapResult bootstrap_repository(
    Storage& storage,
    const FrontendRegistry& registry,
    const std::filesystem::path& repo_root,
    const std::filesystem::path& codegraph_dir
);

bool bootstrap_needed(Storage& storage, const std::filesystem::path& codegraph_dir);

ScanOptions scan_options_for_config(
    const std::filesystem::path& repo_root,
    const RepoConfig& config
);

IndexOptions index_options_for_config(
    const std::filesystem::path& repo_root,
    const RepoConfig& config
);

}  // namespace codegraph
