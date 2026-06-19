#include "time_util.h"

#include <ctime>
#include <stdexcept>

namespace codegraph {

std::string current_utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif

    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        throw std::runtime_error("failed to format timestamp");
    }
    return buffer;
}



}  // namespace codegraph
