// Fixture for unsafe_c_funcs check. Each @expect marker corresponds to a
// following function call that must trip the rule.
#include <cstring>
#include <cstdio>

// NOTE: clangd will flag the unsafe input routine below as undeclared —
// that's expected; the fixture isn't built, only grep'd.

void unsafe_copy(char *dst, const char *src) {
    strcpy(dst, src);      // @expect unsafe_c_funcs
    strcat(dst, "x");      // @expect unsafe_c_funcs
}

void unsafe_format(char *buf) {
    sprintf(buf, "hello"); // @expect unsafe_c_funcs
}

void unsafe_input(char *buf) {
    gets(buf);             // @expect unsafe_c_funcs
    scanf("%s", buf);      // @expect unsafe_c_funcs
}
