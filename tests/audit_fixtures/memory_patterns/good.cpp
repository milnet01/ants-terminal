// Safe allocation patterns — MUST NOT match the memory_patterns grep pattern
// directly. Raw heap-allocation tokens are only tripped when they appear as
// standalone words; this file is structured so comments and code alike avoid
// the literal tokens. Comment convention: UPPER-CASE spellings so case-
// sensitive \b<word> boundaries don't fire.
#include <vector>

struct Widget { int x; };

void by_value() {
    Widget w{};
    (void)w;
}

void by_vector() {
    std::vector<Widget> ws(4);
    (void)ws;
}

// Smart-pointer helpers spell without a trailing space, and raw heap calls
// (MALLOC / CALLOC / REALLOC) are described in UPPER-CASE to avoid matching
// the case-sensitive word-boundary pattern.
