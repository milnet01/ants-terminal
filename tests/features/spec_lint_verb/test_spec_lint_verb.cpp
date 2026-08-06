// ANTS-3662 — spec_lint VERB conformance test (INV-4, INV-7). The handler needs
// a live MainWindow, so behavioural rows drive the pure helpers and wiring rows
// source-scrape the registration sites (the rc_get_text_byte_cap pattern, as
// ANTS-3601's and ANTS-3661's verb tests do).

#include "remotecontrol.h"
#include "speclint.h"

#include <string>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH) || !defined(SRC_SPECLINT_CPP_PATH)
#error "spec_lint_verb test needs the test_claude source-path compile defs"
#endif

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-4 — command_test_no_expectation is a CANDIDATE: never auto-fixable, and
// the verb runs no subprocess. The scrape is the half a behavioural test cannot
// hold: "this engine never executes a clause" is a claim about code that does
// not exist, and only a source scan can assert an absence.
TEST(SpecLintVerb, Inv4CommandClauseIsACandidate) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — clauses\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a. *Test:* `grep -c foo src/`\n"
        "- **INV-2** — b. *Test:* `grep -c foo src/` → 3\n"
        "- **INV-3** — c. *Test:* `tests/features/spec_lint/` covers it\n"
        "- **INV-4** — d. *Test:* run `ctest -R spec_lint`.\n"
        "- **INV-5** — e. *Test:* `tests/features/spec_lint/`\n");
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("c.md"), {});

    int candidates = 0;
    for (const auto &f : r.findings) {
        if (f.kind != QLatin1String("command_test_no_expectation")) continue;
        ++candidates;
        EXPECT_FALSE(f.autoFixable) << "a candidate is never auto-fixable";
        EXPECT_TRUE(f.message.contains(QStringLiteral("candidate")));
    }
    // INV-1 and INV-4 fire: a bare command, and a command whose only trailing
    // text is punctuation. Three do not, and each is excluded by a DIFFERENT
    // half of the heuristic — which is the point of carrying all three:
    //   INV-2  `→ 3` is an expectation (the trailing-alphanumeric rule).
    //   INV-3  trailing prose, so the expectation rule excludes it whatever the
    //          vocabulary says — it does NOT test the vocabulary.
    //   INV-5  a BARE path span with nothing after it. Only the fixed
    //          vocabulary keeps this quiet; a shape test ("contains a slash")
    //          fires here. Without this clause the vocabulary is untested.
    EXPECT_EQ(candidates, 2);

    // The wire form: ANTS-3664 INV-1 OMITS auto_fixable when false, so the
    // assertion is that the key is ABSENT — a test written against
    // `auto_fixable:false` fails on a conforming serialiser.
    DocFinding::Finding f;
    f.verb = QStringLiteral("spec_lint");
    f.kind = QStringLiteral("command_test_no_expectation");
    f.file = QStringLiteral("c.md");
    f.line = 5;
    const QJsonObject wire = DocFinding::toJson(f);
    EXPECT_FALSE(wire.contains(QStringLiteral("auto_fixable")));

    // No subprocess, ever. /write-spec Step 3 owns running a clause, at write
    // time, where a failure is free.
    //
    // COMMENTS ARE STRIPPED FIRST. speclint.cpp cites `QProcess` as an example
    // of a code span that is NOT a command — a comment explaining this very
    // rule — and a raw scrape reads that as the violation. Renaming the example
    // would leave the trap armed for the next comment; the scan is over code.
    const std::string eng =
        ants_test::stripComments(ants_test::slurpFile(SRC_SPECLINT_CPP_PATH));
    ASSERT_FALSE(eng.empty());
    for (const char *banned : {"QProcess", "std::system", "popen", "execve"})
        EXPECT_EQ(eng.find(banned), std::string::npos)
            << "speclint.cpp must never execute a test clause: " << banned;
}

// INV-7 — the verb-contract minimum: caller_cwd Required; a root-escaping
// `path` refuses bad_path; a well-formed non-existent in-root `path` is ok:true
// with empty findings (ANTS-3601 INV-15's shape).
TEST(SpecLintVerb, Inv7RefusalMinimums) {
    // (1) caller_cwd Required — at the call site AND in the static table
    // registerToolProvider asserts against.
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"spec_lint\""));
    ASSERT_GE(reg, 0);
    EXPECT_TRUE(mw.mid(reg, 160).contains(QStringLiteral("CallerCwdContract::Required")));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"spec_lint\")"));
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains(QStringLiteral("C::Required")));
    EXPECT_TRUE(ci.contains(QStringLiteral("specLint[\"name\"] = \"spec_lint\"")));

    // (2) a supplied path is validated BEFORE any enumeration → bad_path.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const int h = rc.indexOf(QStringLiteral("RemoteControl::cmdSpecLint"));
    ASSERT_GE(h, 0);
    const QString handler = rc.mid(h, 1600);
    EXPECT_TRUE(handler.contains(QStringLiteral("validatePath(")));
    EXPECT_TRUE(handler.contains(QStringLiteral("check.err")));

    // (3) nothing to scan → ok:true with EMPTY findings, not a refusal.
    const QJsonObject empty =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {});
    EXPECT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(empty.value(QStringLiteral("findings")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("checked_docs")).toArray().isEmpty());

    // sections_checked is ALWAYS present, never omitted when false — it is the
    // one field distinguishing "every required section is there" from "nobody
    // checked", and false is this verb's shipping default.
    EXPECT_TRUE(empty.contains(QStringLiteral("sections_checked")));
    EXPECT_FALSE(empty.value(QStringLiteral("sections_checked")).toBool());
    EXPECT_TRUE(empty.contains(QStringLiteral("line_count")));
    // truncated is omitted when false (§ 2.1's envelope).
    EXPECT_FALSE(empty.contains(QStringLiteral("truncated")));
}
