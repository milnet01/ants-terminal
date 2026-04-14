// Good-side canary for weak_crypto. Uppercase spellings (MD5, SHA-1, DES,
// RC4, ECB) are used in comments because the rule regex is case-sensitive.
// Identifiers embed the token without a word boundary on the right, so
// \b...\b cannot anchor.

int md5sum_count;            // MD5 prefix continues into `sum` (no boundary)
int describes_things;        // DES substring inside a longer word
int ecbs_internal;            // ECB prefix continues into `s` (no boundary)
void use_SHA1(void);         // uppercase — regex targets lowercase only
void use_MD5(void);          // uppercase — regex targets lowercase only

int main(void) {
    return md5sum_count + describes_things + ecbs_internal;
}
