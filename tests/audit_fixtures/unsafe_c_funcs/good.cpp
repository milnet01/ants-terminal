// Safe replacements — MUST NOT match the unsafe_c_funcs rule.
#include <cstring>
#include <cstdio>

void safe_copy(char *dst, const char *src, size_t n) {
    strncpy(dst, src, n);
}

void safe_format(char *buf, size_t n) {
    snprintf(buf, n, "hello");
}

void safe_input(char *buf, size_t n) {
    fgets(buf, n, stdin);
}
