// Good-side canary for `hardcoded_ips`. Anything that's NOT exactly four
// dotted 1-3 digit groups must not match.

#include <stdio.h>

// Three groups only — version string shape
static const char *VERSION = "1.2.3";

// Hex bytes separated by commas — not dots
static const unsigned char MAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

// Large integer literal — no dots
static unsigned long COUNT = 1234567890;

int main(void) {
    printf("v=%s\n", VERSION);
    return MAC[0];
}
