#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "extraction.h"

namespace codegraph {

class LanguageFrontend {
public:
    virtual ~LanguageFrontend() = default;

    virtual std::string_view language() const = 0;
    virtual std::span<const std::string_view> extensions() const = 0;
    virtual ExtractedFile extract(std::string_view source) const = 0;
};

class FrontendRegistry {
public:
    void add(std::unique_ptr<LanguageFrontend> frontend);
    const LanguageFrontend* for_extension(std::string_view ext) const;
    const LanguageFrontend* for_language(std::string_view language) const;
    std::span<const std::unique_ptr<LanguageFrontend>> all() const;

private:
    std::vector<std::unique_ptr<LanguageFrontend>> frontends_;
};

}  // namespace codegraph
