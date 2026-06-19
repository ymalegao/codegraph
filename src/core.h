#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace codegraph {

enum class NodeId : uint32_t { Invalid = 0xFFFFFFFFu };
enum class FileId : uint32_t {};
enum class StringId : uint32_t {};

enum class NodeKind : uint8_t { File, Symbol, Correction, ArchDecision };
enum class SymbolKind : uint8_t {
    Function,
    Method,
    Class,
    Struct,
    Namespace,
    Enum,
    Field,
    Other
};
enum class EdgeKind : uint8_t { Contains, Imports, Affects };
enum class Status : uint8_t { Active, Tombstoned, Stale };

struct Node {
    NodeKind kind;
    Status status;
    uint16_t flags;
    StringId title;
    uint32_t payload;
};

struct SourceSpan {
    uint32_t start_line;
    uint32_t end_line;
    uint32_t start_byte;
    uint32_t end_byte;
    uint64_t content_hash;
    uint16_t flags;
};

struct PackedSourceSpan {
    std::array<uint64_t, 3> words;
    uint16_t flags;
};

constexpr PackedSourceSpan pack_source_span(SourceSpan span) {
    return PackedSourceSpan{{
                                (static_cast<uint64_t>(span.start_line) << 32U) |
                                    static_cast<uint64_t>(span.end_line),
                                (static_cast<uint64_t>(span.start_byte) << 32U) |
                                    static_cast<uint64_t>(span.end_byte),
                                span.content_hash,
                            },
                            span.flags};
}

constexpr SourceSpan unpack_source_span(PackedSourceSpan packed) {
    return SourceSpan{
        static_cast<uint32_t>(packed.words[0] >> 32U),
        static_cast<uint32_t>(packed.words[0] & 0xFFFFFFFFULL),
        static_cast<uint32_t>(packed.words[1] >> 32U),
        static_cast<uint32_t>(packed.words[1] & 0xFFFFFFFFULL),
        packed.words[2],
        packed.flags,
    };
}

struct SymbolData {
    FileId file;
    SymbolKind sym_kind;
    StringId qualified_name;
    StringId signature;
    SourceSpan span;
};

struct MemoryData {
    uint8_t memory_type;
    int64_t body_rowid;
};

struct Csr {
    std::vector<uint32_t> offsets;
    std::vector<NodeId> neighbors;
    std::vector<EdgeKind> kinds;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<SymbolData> symbols;
    std::vector<MemoryData> memos;
    Csr forward;
    Csr reverse;
};

class StringInterner {
public:
    StringInterner();

    StringId intern(std::string_view text);
    std::string_view view(StringId id) const;

    uint32_t size() const;

private:
    std::vector<char> blob_;
    std::vector<uint32_t> starts_;
    std::unordered_map<std::string_view, StringId> lookup_;

    void rebuild_lookup();
};

constexpr uint32_t to_u32(NodeId id) {
    return static_cast<uint32_t>(id);
}

constexpr uint32_t to_u32(FileId id) {
    return static_cast<uint32_t>(id);
}

constexpr uint32_t to_u32(StringId id) {
    return static_cast<uint32_t>(id);
}

}  // namespace codegraph
