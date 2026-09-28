#pragma once
// Environment variables. Spanly never changes its own environment, so reading it from any thread
// is safe (getenv is only unsafe next to setenv/putenv).

#include <cstdlib>
#include <optional>
#include <string>

namespace spanly {

/// The variable's value, or nothing if it isn't set or is empty.
inline std::optional<std::string> env(const char* name) {
#ifdef _WIN32
    char* v = nullptr;
    size_t size = 0;
    if (_dupenv_s(&v, &size, name) != 0 || !v) return std::nullopt;
    std::string value(v);
    std::free(v); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
    if (value.empty()) return std::nullopt;
    return value;
#else
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): see above
    if (!v || !*v) return std::nullopt;
    return std::string(v);
#endif
}

} // namespace spanly
