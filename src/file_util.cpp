#include "file_util.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace codegraph {

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace codegraph
