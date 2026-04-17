// Fixture: hardcoded-secrets scan. The rule's deployed invocation applies a
// common-noise filter that drops test/mock lines — this fixture harness
// exercises only the raw regex. 2026-04-16: the regex was tightened to
// require the RHS to be a literal string of ≥16 characters, so test
// secrets here are long enough to trip it.
const char *api_key = "AIzaSy_placeholder_16chars_long_X";     // @expect secrets_scan
const char *password = "hunter2_for_testing_only_placeholder"; // @expect secrets_scan
const char *auth_token = "ghp_abcdef0123456789_placeholder";   // @expect secrets_scan
const char *credentials = "user:pass_credential_16chars_X";    // @expect secrets_scan
