// Good-side canary: every printf-family call carries a literal format
// string, so the `[^"]*` clause in the regex cannot cross the first `"`.
// Also avoid parenthesised word expressions (`sizeof(buf)`) inside the
// pre-quote region — those would otherwise satisfy `\b\w+\s*\)`.

#include <stdio.h>

int main(void) {
    char buf[64];
    printf("hello\n");
    fprintf(stderr, "error: %d\n", 42);
    snprintf(buf, 64, "%d", 7);
    return 0;
}
