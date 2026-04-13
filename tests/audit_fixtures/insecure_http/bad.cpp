// Fixture: insecure_http rule.
const char *backend = "http://api.example.com";    // @expect insecure_http
const char *cdn     = "http://cdn.example.org/x";  // @expect insecure_http
