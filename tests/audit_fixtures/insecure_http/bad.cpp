// Fixture: insecure_http rule.
const char *backend = "http://api.example.com";    // @expect insecure_http
const char *cdn     = "http://cdn.example.org/x";  // @expect insecure_http
// l-initial host the old positional-class hack ([^l][^o][^c]) silently
// missed; the bare plain-scheme regex now flags it (ANTS-1612).
const char *logs    = "http://logging.internal";   // @expect insecure_http
