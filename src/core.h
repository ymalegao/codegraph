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
enum class MemoryType : uint8_t { Correction = 0, ArchDecision = 1, Unknown = 255 };

namespace KindText {
inline constexpr std::string_view File = "file";
inline constexpr std::string_view Symbol = "symbol";
inline constexpr std::string_view Correction = "correction";
inline constexpr std::string_view ArchDecision = "arch_decision";
inline constexpr std::string_view Function = "function";
inline constexpr std::string_view Method = "method";
inline constexpr std::string_view Class = "class";
inline constexpr std::string_view Struct = "struct";
inline constexpr std::string_view Namespace = "namespace";
inline constexpr std::string_view Enum = "enum";
inline constexpr std::string_view Field = "field";
inline constexpr std::string_view Other = "other";
inline constexpr std::string_view Contains = "contains";
inline constexpr std::string_view Imports = "imports";
inline constexpr std::string_view Affects = "affects";
inline constexpr std::string_view Active = "active";
inline constexpr std::string_view Tombstoned = "tombstoned";
inline constexpr std::string_view Stale = "stale";
}  // namespace KindText

constexpr std::string_view node_kind_text(NodeKind kind) {
    switch (kind) {
        case NodeKind::File: return KindText::File;
        case NodeKind::Symbol: return KindText::Symbol;
        case NodeKind::Correction: return KindText::Correction;
        case NodeKind::ArchDecision: return KindText::ArchDecision;
    }
    return KindText::File;
}

constexpr std::string_view symbol_kind_text(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Function: return KindText::Function;
        case SymbolKind::Method: return KindText::Method;
        case SymbolKind::Class: return KindText::Class;
        case SymbolKind::Struct: return KindText::Struct;
        case SymbolKind::Namespace: return KindText::Namespace;
        case SymbolKind::Enum: return KindText::Enum;
        case SymbolKind::Field: return KindText::Field;
        case SymbolKind::Other: return KindText::Other;
    }
    return KindText::Other;
}

constexpr std::string_view edge_kind_text(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Contains: return KindText::Contains;
        case EdgeKind::Imports: return KindText::Imports;
        case EdgeKind::Affects: return KindText::Affects;
    }
    return KindText::Contains;
}

constexpr std::string_view status_text(Status status) {
    switch (status) {
        case Status::Active: return KindText::Active;
        case Status::Tombstoned: return KindText::Tombstoned;
        case Status::Stale: return KindText::Stale;
    }
    return KindText::Active;
}

constexpr std::string_view memory_type_text(MemoryType type) {
    switch (type) {
        case MemoryType::Correction: return KindText::Correction;
        case MemoryType::ArchDecision: return KindText::ArchDecision;
        case MemoryType::Unknown: return KindText::Other;
    }
    return KindText::Other;
}

NodeKind node_kind_from_string(std::string_view kind);
SymbolKind symbol_kind_from_string(std::string_view kind);
EdgeKind edge_kind_from_string(std::string_view kind);
Status status_from_string(std::string_view status);
MemoryType memory_type_from_string(std::string_view type);

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
    StringId name;
    StringId qualified_name;
    StringId signature;
    SourceSpan span;
};

struct MemoryData {
    MemoryType memory_type;
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
