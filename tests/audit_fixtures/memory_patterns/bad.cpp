// Fixture for memory_patterns rule — raw C/C++ allocation tokens. Each
// @expect marker is a site that's not paired with smart-ptr / Qt parent-child
// ownership (those are filtered by dropIfContains in the audit dialog, not in
// the regex, so the grep still counts them here). Comments intentionally
// avoid the literal tokens so they don't inflate the match count.
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
