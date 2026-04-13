// Safe: localhost + https.
const char *dev    = "http://localhost:8080";
const char *prod   = "https://api.example.com";
// Namespace/schema URLs are excluded from the real check via dropIfContains,
// but this fixture file tests only the pattern regex, which should NOT match
// localhost (the regex explicitly excludes a leading loc prefix).
