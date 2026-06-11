// Fixture for memory_patterns rule — raw C/C++ allocation tokens. Each
// flagged site is one that's not paired with smart-ptr / Qt parent-child
// ownership (those are filtered by dropIfContains in the audit dialog, not in
// the regex, so the grep still counts them here). Comments intentionally
// avoid the literal tokens (and the literal "@expect <id>" form, except on
// the inline markers below) so they don't inflate the match count.
#include <cstdlib>

struct Widget { int x; };

void leaky_raw_new() {
    Widget *w = new Widget();  // @expect memory_patterns
    (void)w;
}

void leaky_malloc() {
    char *buf = (char *)malloc(128);      // @expect memory_patterns
    void *b2  = calloc(1, 128);           // @expect memory_patterns
    void *b3  = realloc(b2, 256);         // @expect memory_patterns
    (void)buf; (void)b3;
}

// Single-pointer-arg ctor forms — the regex's optional (nullptr|NULL)
// alternative must still flag these (ANTS-1478 coverage gap).
void leaky_ptr_ctor() {
    Widget *wn = new Widget(nullptr);     // @expect memory_patterns
    Widget *wN = new Widget(NULL);        // @expect memory_patterns
    (void)wn; (void)wN;
}
