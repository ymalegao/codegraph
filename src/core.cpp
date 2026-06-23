#include "core.h"

#include "memory_kinds.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace codegraph {
namespace {

constexpr size_t kMaxInternedStringBytes = 4096;

uint32_t checked_u32_size(size_t value, const char* label) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error(std::string(label) + " exceeds uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

}  // namespace

NodeKind node_kind_from_string(std::string_view kind) {
    if (kind == KindText::File) return NodeKind::File;
    if (kind == KindText::Symbol) return NodeKind::Symbol;
    if (const MemoryKind* memory_kind = memory_kind_by_text(kind)) {
        return memory_kind->node_kind;
    }
    throw std::runtime_error("unknown node kind: " + std::string(kind));
}

SymbolKind symbol_kind_from_string(std::string_view kind) {
    if (kind == KindText::Function) return SymbolKind::Function;
    if (kind == KindText::Method) return SymbolKind::Method;
    if (kind == KindText::Class) return SymbolKind::Class;
    if (kind == KindText::Struct) return SymbolKind::Struct;
    if (kind == KindText::Namespace) return SymbolKind::Namespace;
    if (kind == KindText::Enum) return SymbolKind::Enum;
    if (kind == KindText::Field) return SymbolKind::Field;
    return SymbolKind::Other;
}

EdgeKind edge_kind_from_string(std::string_view kind) {
    if (kind == KindText::Contains) return EdgeKind::Contains;
    if (kind == KindText::Imports) return EdgeKind::Imports;
    if (kind == KindText::Affects) return EdgeKind::Affects;
    throw std::runtime_error("unknown edge kind: " + std::string(kind));
}

Status status_from_string(std::string_view status) {
    if (status == KindText::Active) return Status::Active;
    if (status == KindText::Tombstoned) return Status::Tombstoned;
    if (status == KindText::Stale) return Status::Stale;
    return Status::Active;
}

MemoryType memory_type_from_string(std::string_view type) {
    if (const MemoryKind* memory_kind = memory_kind_by_text(type)) {
        return memory_kind->type;
    }
    return MemoryType::Unknown;
}

std::string_view node_kind_text(NodeKind kind) {
    if (kind == NodeKind::File) {
        return KindText::File;
    }
    if (kind == NodeKind::Symbol) {
        return KindText::Symbol;
    }
    if (const MemoryKind* memory_kind = memory_kind_by_node_kind(kind)) {
        return memory_kind->text;
    }
    return KindText::File;
}

std::string_view memory_type_text(MemoryType type) {
    if (type == MemoryType::Unknown) {
        return KindText::Other;
    }
    if (const MemoryKind* memory_kind = memory_kind_by_memory_type(type)) {
        return memory_kind->text;
    }
    return KindText::Other;
}

StringInterner::StringInterner() : starts_{0} {}

StringId StringInterner::intern(std::string_view text) {
    if (text.size() > kMaxInternedStringBytes) {
        throw std::invalid_argument("string is too large to intern");
    }

    if (const auto existing = lookup_.find(text); existing != lookup_.end()) {
        return existing->second;
    }

    const auto id = static_cast<StringId>(checked_u32_size(starts_.size() - 1, "string id"));
    const size_t old_capacity = blob_.capacity();

    blob_.insert(blob_.end(), text.begin(), text.end());
    starts_.push_back(checked_u32_size(blob_.size(), "interner blob"));

    if (blob_.capacity() != old_capacity) {
        rebuild_lookup();
    } else {
        lookup_.emplace(view(id), id);
    }

    return id;
}

std::string_view StringInterner::view(StringId id) const {
    const uint32_t index = to_u32(id);
    if (index + 1 >= starts_.size()) {
        throw std::out_of_range("invalid StringId");
    }

    const uint32_t start = starts_[index];
    const uint32_t end = starts_[index + 1];
    const char* base = blob_.empty() ? "" : blob_.data();
    return std::string_view(base + start, end - start);
}

uint32_t StringInterner::size() const {
    return checked_u32_size(starts_.size() - 1, "interner size");
}

void StringInterner::rebuild_lookup() {
    lookup_.clear();
    lookup_.reserve(starts_.size() - 1);
    for (uint32_t i = 0; i + 1 < starts_.size(); ++i) {
        const auto id = static_cast<StringId>(i);
        lookup_.emplace(view(id), id);
    }
}

}  // namespace codegraph
