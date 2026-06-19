#include "frontend.h"

#include <stdexcept>

namespace codegraph {

void FrontendRegistry::add(std::unique_ptr<LanguageFrontend> frontend) {
    if (!frontend) {
        throw std::invalid_argument("cannot register null language frontend");
    }
    frontends_.push_back(std::move(frontend));
}

const LanguageFrontend* FrontendRegistry::for_extension(std::string_view ext) const {
    for (const auto& frontend : frontends_) {
        for (const std::string_view candidate : frontend->extensions()) {
            if (candidate == ext) {
                return frontend.get();
            }
        }
    }
    return nullptr;
}

const LanguageFrontend* FrontendRegistry::for_language(std::string_view language) const {
    for (const auto& frontend : frontends_) {
        if (frontend->language() == language) {
            return frontend.get();
        }
    }
    return nullptr;
}

std::span<const std::unique_ptr<LanguageFrontend>> FrontendRegistry::all() const {
    return frontends_;
}

}  // namespace codegraph
