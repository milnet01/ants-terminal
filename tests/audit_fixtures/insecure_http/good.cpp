// Safe: only https here. The insecure_http regex is now the bare plain
// scheme match (ANTS-1612); localhost / 127.0.0.1 / schema URLs are
// excluded by the runtime OutputFilter (dropIfContains), NOT by the regex,
// so this regex-only fixture must contain zero plain-scheme URLs.
const char *prod   = "https://api.example.com";
const char *secure = "https://localhost:8443";
