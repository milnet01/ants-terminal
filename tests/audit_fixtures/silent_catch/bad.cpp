// Silent-handler fixtures — the audit rule flags empty same-line catch bodies
// because they swallow exceptions without logging or rethrow. See
// ROADMAP.md §0.7 Dev experience / CHANGELOG §0.6.7.
//
// Each @expect marker on a line with an empty catch body asserts the rule
// pattern matches exactly there. Comments here describe the rule paraphrastically
// so the case-sensitive regex doesn't fire on this commentary.

#include <exception>
#include <stdexcept>
#include <string>

int parse_int_empty(const char *s) {
    try {
        return std::stoi(s);
    } catch (...) {}  // @expect silent_catch
    return 0;
}

int parse_long_empty(const char *s) {
    try {
        return std::stol(s);
    } catch (const std::exception &) { }  // @expect silent_catch
    return 0;
}

void run_empty(void (*fn)()) {
    try {
        fn();
    } catch (std::runtime_error&){}  // @expect silent_catch
}
