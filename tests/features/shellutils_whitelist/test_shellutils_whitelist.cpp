// Feature-conformance test for tests/features/shellutils_whitelist/spec.md.
//
// INV-1 empty input → ''
// INV-2 whitelist (`[A-Za-z0-9_\-./:@%+,]`) returns input unchanged
// INV-3 anything outside the whitelist forces single-quote wrapping
// INV-4 embedded single quote round-trips as the POSIX `'\''` form
//
// Pure unit test against shellutils.h; links Qt6::Core only.

#include "shellutils.h"

#include <QString>
#include <cstdio>

namespace {

int fail(const char *label, const QString &got, const char *expected) {
    std::fprintf(stderr, "[%s] FAIL: got=\"%s\" expected=\"%s\"\n",
                 label, qUtf8Printable(got), expected);
    return 1;
}

int expectEq(const char *label, const QString &got, const char *expected) {
    if (got == QString::fromUtf8(expected)) return 0;
    return fail(label, got, expected);
}

}  // namespace

int main() {
    int rc = 0;

    // INV-1: empty input
    rc |= expectEq("INV-1", shellQuote(QStringLiteral("")), "''");

    // INV-2: safe-character whitelist passes through unchanged
    rc |= expectEq("INV-2/a", shellQuote(QStringLiteral("/home/user/project")),
                   "/home/user/project");
    rc |= expectEq("INV-2/b", shellQuote(QStringLiteral("foo-bar.txt")),
                   "foo-bar.txt");
    rc |= expectEq("INV-2/c", shellQuote(QStringLiteral("user@host.com:22")),
                   "user@host.com:22");
    rc |= expectEq("INV-2/d", shellQuote(QStringLiteral("value%20enc+stuff,more")),
                   "value%20enc+stuff,more");
    rc |= expectEq("INV-2/e", shellQuote(QStringLiteral("plain")), "plain");

    // INV-3: characters NOT in the whitelist force quoting. Each of
    // these would have slipped through the pre-0.7.57 denylist.
    rc |= expectEq("INV-3/star",   shellQuote(QStringLiteral("/tmp/foo*")),
                   "'/tmp/foo*'");
    rc |= expectEq("INV-3/qmark",  shellQuote(QStringLiteral("/tmp/?ar")),
                   "'/tmp/?ar'");
    rc |= expectEq("INV-3/lbrack", shellQuote(QStringLiteral("/tmp/[a-z]")),
                   "'/tmp/[a-z]'");
    rc |= expectEq("INV-3/lt",     shellQuote(QStringLiteral("name<input.txt")),
                   "'name<input.txt'");
    rc |= expectEq("INV-3/gt",     shellQuote(QStringLiteral("name>output.txt")),
                   "'name>output.txt'");

    // INV-3 (existing-class): characters the old denylist DID catch
    // still get quoted in the new whitelist.
    rc |= expectEq("INV-3/space", shellQuote(QStringLiteral("a b")), "'a b'");
    rc |= expectEq("INV-3/dollar", shellQuote(QStringLiteral("$HOME")),
                   "'$HOME'");
    rc |= expectEq("INV-3/pipe", shellQuote(QStringLiteral("a|b")), "'a|b'");

    // INV-4: embedded single quote → POSIX 'X'\''Y' shape
    rc |= expectEq("INV-4", shellQuote(QStringLiteral("it's")), "'it'\\''s'");

    if (rc == 0) std::fprintf(stdout, "shellutils_whitelist: all passed\n");
    return rc;
}
