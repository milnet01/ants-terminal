// Good-side canary for `hardcoded_ips`. Anything that's NOT four dotted
// valid-octet (0-255) groups must not match.

#include <stdio.h>

// Three groups only — version string shape
static const char *VERSION = "1.2.3";

// ANTS-1710: four dotted groups but a component >255 — a build number, not an
// IPv4 address. The valid-octet regex must NOT match these.
static const char *BUILD     = "1.2.300.4";
static const char *BUILD2    = "10.20.999.0";

// Hex bytes separated by commas — not dots
static const unsigned char MAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

// Large integer literal — no dots
static unsigned long COUNT = 1234567890;

int main(void) {
    printf("v=%s\n", VERSION);
    return MAC[0];
}
