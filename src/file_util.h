#pragma once

#include <filesystem>
#include <string>

namespace codegraph {

std::string read_file_bytes(const std::filesystem::path& path);

}  // namespace codegraph
