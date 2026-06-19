#pragma once

#include <string>
#include <string_view>

namespace codegraph {

std::string xxh64_hex(std::string_view bytes);

}  // namespace codegraph
