#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core.h"

namespace codegraph {

namespace SymbolKinds {
inline constexpr const char* Function = KindText::Function.data();
inline constexpr const char* Method = KindText::Method.data();
inline constexpr const char* Class = KindText::Class.data();
inline constexpr const char* Struct = KindText::Struct.data();
inline constexpr const char* Namespace = KindText::Namespace.data();
inline constexpr const char* Enum = KindText::Enum.data();
inline constexpr const char* Field = KindText::Field.data();
inline constexpr const char* Other = KindText::Other.data();
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
