// Fixture: hardcoded-secrets scan. The rule's deployed invocation applies a
// common-noise filter that drops test/mock lines — this fixture harness
// exercises only the raw regex.
const char *api_key = "AIzaSyXYZ";      // @expect secrets_scan
const char *password = "hunter2";        // @expect secrets_scan
const char *auth_token = "ghp_abcdef";   // @expect secrets_scan
const char *credentials = "user:pass";   // @expect secrets_scan
