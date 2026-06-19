#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codegraph {

namespace SymbolKinds {
inline constexpr const char* Function = "function";
inline constexpr const char* Method = "method";
inline constexpr const char* Class = "class";
inline constexpr const char* Struct = "struct";
inline constexpr const char* Namespace = "namespace";
inline constexpr const char* Enum = "enum";
inline constexpr const char* Field = "field";
inline constexpr const char* Other = "other";
}  // namespace SymbolKinds

struct SymbolInfo {
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;
    std::string content_hash;
    int parent_index = -1;
};

struct IncludeInfo {
    std::string target;
};

struct ExtractedFile {
    std::vector<SymbolInfo> symbols;
    std::vector<IncludeInfo> includes;
};

}  // namespace codegraph
