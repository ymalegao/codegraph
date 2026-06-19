#include "hash_util.h"

#include <cstdint>

#include "xxhash.h"

namespace codegraph {

std::string xxh64_hex(std::string_view bytes) {
    uint64_t hash = XXH64(bytes.data(), bytes.size(), 0);
    constexpr char kHex[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[static_cast<size_t>(i)] = kHex[hash & 0xFULL];
        hash >>= 4U;
    }
    return result;
}

}  // namespace codegraph
