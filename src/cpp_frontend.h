#pragma once

#include "frontend.h"

namespace codegraph {

class CppFrontend final : public LanguageFrontend {
public:
    std::string_view language() const override;
    std::span<const std::string_view> extensions() const override;
    ExtractedFile extract(std::string_view source) const override;
};

}  // namespace codegraph
