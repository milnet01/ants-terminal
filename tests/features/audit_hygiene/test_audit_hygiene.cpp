// Feature-conformance test for spec.md — locks the regex-based parsers in
// audithygiene.cpp so ruff/semgrep format drift doesn't silently reopen
// the RetroDB 1.3%-signal-rate problem.
//
// Headless: pure string → list conversion, no QApplication needed.

#include "audithygiene.h"

#include <QString>
#include <QStringList>

#include <cstdio>

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);   \
        ++failures;                                                          \
    }                                                                        \
} while (0)

void expectList(const char *label, const QStringList &actual,
                const QStringList &expected) {
    if (actual == expected) return;
    std::fprintf(stderr, "FAIL %s\n  expected: [%s]\n  actual:   [%s]\n",
                 label,
                 qPrintable(expected.join(", ")),
                 qPrintable(actual.join(", ")));
    ++failures;
}

// ---------------------------------------------------------------------------
// Semgrep invariants 1-4.
// ---------------------------------------------------------------------------

void testSemgrepMissingMarker() {
    const QString yml = R"(# Random header with no marker
rules:
  - id: foo
)";
    expectList("semgrep.missingMarker", AuditHygiene::parseSemgrepExcludeRules(yml), {});
}

void testSemgrepEmpty() {
    expectList("semgrep.empty", AuditHygiene::parseSemgrepExcludeRules(""), {});
}

void testSemgrepHappyPath() {
    const QString yml = R"(# Example config
#
# Excluded upstream rules
# -----------------------
# These IDs are noise-only on this repo.
#
#   python.flask.security.audit.debug-enabled.debug-enabled
#     Anchor: app.py:1238 is env-gated.
#
#   python.lang.security.insecure-hash-algorithms-md5.insecure-hash-algorithm-md5
#     Anchor: API contract requirement.
#
rules: []
)";
    const QStringList got = AuditHygiene::parseSemgrepExcludeRules(yml);
    const QStringList want = {
        "python.flask.security.audit.debug-enabled.debug-enabled",
        "python.lang.security.insecure-hash-algorithms-md5.insecure-hash-algorithm-md5",
    };
    expectList("semgrep.happyPath", got, want);
}

void testSemgrepProseAndSeparatorsIgnored() {
    const QString yml = R"(# Excluded upstream rules
# -----------------------
# This is prose text. Capitalized words. No rule IDs.
# Another prose line, mentioning files like routes/games.py incidentally.
#
#   real.rule.id.one
#     Anchor: ...
#
rules: []
)";
    const QStringList got = AuditHygiene::parseSemgrepExcludeRules(yml);
    expectList("semgrep.proseIgnored", got, {"real.rule.id.one"});
}

void testSemgrepDedup() {
    const QString yml = R"(# Excluded upstream rules
#   same.rule.id
#   same.rule.id
#   other.rule.id
rules: []
)";
    const QStringList got = AuditHygiene::parseSemgrepExcludeRules(yml);
    expectList("semgrep.dedup", got, {"same.rule.id", "other.rule.id"});
}

// ---------------------------------------------------------------------------
// Bandit invariants 5-10.
// ---------------------------------------------------------------------------

void testBanditNoRuffSection() {
    const QString toml = R"([tool.black]
line-length = 100
)";
    expectList("bandit.noRuff", AuditHygiene::parseBanditSkipCodes(toml), {});
}

void testBanditHappyPath() {
    const QString toml = R"([tool.ruff.lint]
select = ["E", "F", "B", "S"]
ignore = [
    "F401",
    "E501",
    "B007",
    "S101",
    "S104",
    "S324",
]
)";
    const QStringList got = AuditHygiene::parseBanditSkipCodes(toml);
    expectList("bandit.happyPath", got, {"B101", "B104", "B324"});
}

void testBanditToolRuffFallback() {
    const QString toml = R"([tool.ruff]
ignore = ["S603", "S607"]
)";
    const QStringList got = AuditHygiene::parseBanditSkipCodes(toml);
    expectList("bandit.toolRuffFallback", got, {"B603", "B607"});
}

void testBanditPrefersLint() {
    // `[tool.ruff]` has S500 with no per-file ignores; `[tool.ruff.lint]`
    // should win with S101/S104. (Per ruff's own docs, `.lint` is the
    // canonical location; the older `[tool.ruff]` placement is legacy.)
    const QString toml = R"([tool.ruff]
line-length = 100
ignore = ["S500"]

[tool.ruff.lint]
ignore = ["S101", "S104"]
)";
    const QStringList got = AuditHygiene::parseBanditSkipCodes(toml);
    expectList("bandit.prefersLint", got, {"B101", "B104"});
}

void testBanditStopsAtNextSection() {
    // A per-file-ignores section after the main ignore block must NOT leak
    // its S-codes into the result. Only the `[tool.ruff.lint]` body's own
    // ignore array counts.
    const QString toml = R"([tool.ruff.lint]
ignore = ["S101"]

[tool.ruff.lint.per-file-ignores]
"tests/*" = ["S999"]
)";
    const QStringList got = AuditHygiene::parseBanditSkipCodes(toml);
    expectList("bandit.stopsAtNextSection", got, {"B101"});
}

void testBanditExtendIgnore() {
    const QString toml = R"([tool.ruff.lint]
extend-ignore = ["S311", "S324"]
)";
    const QStringList got = AuditHygiene::parseBanditSkipCodes(toml);
    expectList("bandit.extendIgnore", got, {"B311", "B324"});
}

} // namespace

int main() {
    testSemgrepMissingMarker();
    testSemgrepEmpty();
    testSemgrepHappyPath();
    testSemgrepProseAndSeparatorsIgnored();
    testSemgrepDedup();

    testBanditNoRuffSection();
    testBanditHappyPath();
    testBanditToolRuffFallback();
    testBanditPrefersLint();
    testBanditStopsAtNextSection();
    testBanditExtendIgnore();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d test(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All audit-hygiene tests passed.\n");
    return 0;
}
