// ANTS-5446 — the perf benchmarks take their settings from environment
// variables. One checked parse serves them all: a value that is not wholly a
// decimal integer, or does not fit an int, reads as unset. std::atoi turned
// "12abc" into 12 and an out-of-range value into undefined behaviour.

#ifndef ANTS_TESTS_PERF_ENV_H
#define ANTS_TESTS_PERF_ENV_H

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <system_error>

namespace AntsPerf {

// The variable's value, or nothing when it is unset, empty or not an int.
inline std::optional<int> envInt(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    const char *end = v + std::strlen(v);
    int n = 0;
    const auto [ptr, ec] = std::from_chars(v, end, n);
    if (ec != std::errc() || ptr != end) return std::nullopt;
    return n;
}

// The variable's value when it is a positive int, else `fallback`.
inline int envPositiveInt(const char *name, int fallback) {
    const std::optional<int> n = envInt(name);
    return n && *n > 0 ? *n : fallback;
}

}  // namespace AntsPerf

#endif  // ANTS_TESTS_PERF_ENV_H
