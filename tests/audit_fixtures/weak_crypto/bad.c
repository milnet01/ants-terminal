// Fixture for the `weak_crypto` audit rule.
//
// Regex targets LOWERCASE tokens (uppercase MD5/SHA-1/DES/RC4/ECB in
// comments deliberately avoided so they don't match the case-sensitive
// regex and inflate expected counts).

int main(void) {
    md5("x", 1);    // @expect weak_crypto
    sha1("y", 2);   // @expect weak_crypto
    des("z");       // @expect weak_crypto
    rc4("w");       // @expect weak_crypto
    ecb();          // @expect weak_crypto
    return 0;
}
