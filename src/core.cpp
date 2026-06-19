#include "core.h"

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
