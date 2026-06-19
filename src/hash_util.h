#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace codegraph {

uint64_t xxh64_u64(std::string_view bytes);
std::string xxh64_hex(std::string_view bytes);

}  // namespace codegraph
