#include "bootstrap.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "file_util.h"
#include "sqlite_util.h"

namespace codegraph {
namespace {

std::string trim(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t' ||
                           text[end - 1U] == '\r' || text[end - 1U] == '\n')) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::string strip_quotes(std::string value) {
    if (value.size() >= 2U &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1U, value.size() - 2U);
    }
    return value;
}

std::vector<std::string> default_ignore_patterns() {
    return {
        ".git/**",
        "build/**",
        "cmake-build-*/**",
        "node_modules/**",
        "**/__pycache__/**",
        "third_party/**",
        "generated/**",
    };
}

std::string default_repo_id(const std::filesystem::path& repo_root) {
    const std::filesystem::path name = std::filesystem::weakly_canonical(repo_root).filename();
    const std::string text = name.string();
    return text.empty() ? "local" : text;
}

void write_default_config(const std::filesystem::path& config_path, const RepoConfig& config) {
    std::ofstream out(config_path);
    if (!out) {
        throw std::runtime_error("failed to write CodeGraph config: " + config_path.string());
    }
    out << "repo_id: " << config.repo_id << "\n";
    out << "ignore:\n";
    for (const std::string& pattern : config.ignore_patterns) {
        out << "  - " << pattern << "\n";
    }
    out << "max_file_size_mb: " << config.max_file_size_mb << "\n";
}

RepoConfig parse_config(const std::filesystem::path& repo_root, std::string_view text) {
    RepoConfig config{default_repo_id(repo_root), default_ignore_patterns(), 10};
    config.ignore_patterns.clear();

    bool in_ignore = false;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (trimmed == "ignore:") {
            in_ignore = true;
            continue;
        }

        if (in_ignore && trimmed.rfind("-", 0) == 0) {
            config.ignore_patterns.push_back(strip_quotes(trim(trimmed.substr(1U))));
            continue;
        }
        in_ignore = false;

        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trim(std::string_view(trimmed).substr(0, colon));
        const std::string value = strip_quotes(trim(std::string_view(trimmed).substr(colon + 1U)));
        if (key == "repo_id" && !value.empty()) {
            config.repo_id = value;
        } else if (key == "max_file_size_mb" && !value.empty()) {
            config.max_file_size_mb = static_cast<size_t>(std::stoul(value));
        }
    }

    if (config.ignore_patterns.empty()) {
        config.ignore_patterns = default_ignore_patterns();
    }
    return config;
}

}  // namespace

RepoConfig load_or_create_config(const std::filesystem::path& repo_root) {
    const std::filesystem::path codegraph_dir = repo_root / ".codegraph";
    const std::filesystem::path config_path = codegraph_dir / "config.yaml";
    std::filesystem::create_directories(codegraph_dir);
    std::filesystem::create_directories(codegraph_dir / "ops");
    std::filesystem::create_directories(codegraph_dir / "logs");

    if (!std::filesystem::exists(config_path)) {
        RepoConfig config{default_repo_id(repo_root), default_ignore_patterns(), 10};
        write_default_config(config_path, config);
        return config;
    }

    return parse_config(repo_root, read_file_bytes(config_path));
}

ScanOptions scan_options_for_config(
    const std::filesystem::path& repo_root,
    const RepoConfig& config
) {
    return ScanOptions{
        repo_root,
        config.repo_id,
        config.max_file_size_mb * 1024U * 1024U,
        config.ignore_patterns,
    };
}

IndexOptions index_options_for_config(
    const std::filesystem::path& repo_root,
    const RepoConfig& config
) {
    return IndexOptions{repo_root, config.repo_id};
}

BootstrapResult bootstrap_repository(
    Storage& storage,
    const FrontendRegistry& registry,
    const std::filesystem::path& repo_root,
    const std::filesystem::path& codegraph_dir
) {
    std::filesystem::create_directories(codegraph_dir);
    std::filesystem::create_directories(codegraph_dir / "ops");
    std::filesystem::create_directories(codegraph_dir / "logs");
    RepoConfig config = load_or_create_config(repo_root);
    (void)ensure_device_id(codegraph_dir);

    storage.initialize_schema();
    ScanResult scan = scan_repository(storage, registry, scan_options_for_config(repo_root, config));
    IndexResult index = index_repository(storage, registry, index_options_for_config(repo_root, config));
    MaterializeResult materialized = materialize(storage, codegraph_dir);
    return BootstrapResult{std::move(config), scan, index, materialized};
}

bool bootstrap_needed(Storage& storage, const std::filesystem::path& codegraph_dir) {
    if (!std::filesystem::exists(codegraph_dir)) {
        return true;
    }
    storage.initialize_schema();
    return storage.query_int("SELECT COUNT(*) FROM files;") == 0;
}

}  // namespace codegraph
