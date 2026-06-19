#pragma once

#include <cstdint>
#include <string_view>

#include "storage.h"

namespace codegraph {

int64_t resolve_reference(Storage& storage, std::string_view ref);
uint32_t resolver_pass(Storage& storage);

}  // namespace codegraph
