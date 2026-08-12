// ANTS-3662 — spec_lint VERB conformance test (INV-4, INV-7). The handler needs
// a live MainWindow, so behavioural rows drive the pure helpers and wiring rows
// source-scrape the registration sites (the rc_get_text_byte_cap pattern, as
// ANTS-3601's and ANTS-3661's verb tests do).

#include "remotecontrol.h"
#include "speclint.h"

#include <string>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH) || !defined(SRC_SPECLINT_CPP_PATH)
#error "spec_lint_verb test needs the test_claude source-path compile defs"
#endif
#if !defined(ANTS_SOURCE_DIR)
#error "spec_lint_verb test needs ANTS_SOURCE_DIR for the live gathering check"
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
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, false);
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

// ANTS-4127 INV-3..5, INV-7, INV-10 at the WIRE, plus the gathering step. The
// engine lane proves the classification; this lane proves it survives
// serialisation and that the verb actually gathers what the engine is handed —
// an engine that resolves perfectly against sets nobody fills reports a clean
// corpus forever.
TEST(SpecLintVerb, Ants4127SurfaceFieldsReachTheWire) {
    // (1) both envelope fields are ALWAYS emitted. `surfaces_checked` is the one
    // § 2.3 calls "always emitted and never inferred", and this is the only
    // layer where that is observable — a verb lane that never asserts it leaves
    // the claim untested.
    const QJsonObject empty =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, false);
    ASSERT_TRUE(empty.contains(QStringLiteral("surfaces_checked")));
    EXPECT_FALSE(empty.value(QStringLiteral("surfaces_checked")).toBool());
    ASSERT_TRUE(empty.contains(QStringLiteral("surfaces_resolved")));
    EXPECT_EQ(empty.value(QStringLiteral("surfaces_resolved")).toInt(), 0);

    // The falsifying pair: checked TRUE with the counter at ZERO. An envelope
    // built by inferring the flag from the count cannot produce this row.
    const QJsonObject checked =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, true);
    EXPECT_TRUE(checked.value(QStringLiteral("surfaces_checked")).toBool());
    EXPECT_EQ(checked.value(QStringLiteral("surfaces_resolved")).toInt(), 0);

    // (2) the three kinds, with their per-kind detail, through DocFinding's
    // serialiser — including `spec_status: null`, which must be a PRESENT key
    // holding null rather than an omitted one (§ 2.3).
    const QString doc = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a rule. *Test:* `tests/features/absent_one/` covers it.\n");
    SpecLint::Options opts;
    opts.existingTestDirs = {QStringLiteral("present_one")};
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("s.md"), opts);
    ASSERT_EQ(r.findings.size(), 1);
    const QJsonObject wire = DocFinding::toJson(r.findings.first());
    EXPECT_EQ(wire.value(QStringLiteral("kind")).toString(),
              QStringLiteral("test_surface_unresolved"));
    EXPECT_EQ(wire.value(QStringLiteral("invariant")).toString(),
              QStringLiteral("INV-1"));
    EXPECT_EQ(wire.value(QStringLiteral("surface")).toString(),
              QStringLiteral("tests/features/absent_one"));
    ASSERT_TRUE(wire.contains(QStringLiteral("spec_status")));
    EXPECT_TRUE(wire.value(QStringLiteral("spec_status")).isNull());
    // The six base fields are the contract ANTS-3663 sorts on and `extra` must
    // never redefine one.
    EXPECT_EQ(wire.value(QStringLiteral("verb")).toString(),
              QStringLiteral("spec_lint"));
    EXPECT_EQ(wire.value(QStringLiteral("file")).toString(), QStringLiteral("s.md"));

    // (3) the gathering step is wired into the handler, once per run and before
    // the document walk — the shape specLintRequiredSections already uses.
    // The WHOLE body, not a fixed byte window: this handler is ~130 lines and
    // the accumulation sits at the end of the document loop, so a `mid(h, N)`
    // window silently stops testing as the body grows (the ANTS-1348 trap
    // `slurpFunctionBody` exists for).
    const std::string rcSrc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rcSrc.empty());
    const QString handler = QString::fromStdString(
        ants_test::slurpFunctionBody(rcSrc, "RemoteControl::cmdSpecLint"));
    ASSERT_FALSE(handler.isEmpty());
    EXPECT_TRUE(handler.contains(QStringLiteral("opts.existingTestDirs =")));
    EXPECT_TRUE(handler.contains(QStringLiteral("opts.wiredTestDirs    =")));
    EXPECT_TRUE(handler.contains(QStringLiteral("surfacesResolved += r.surfacesResolved")))
        << "a walk's total is the SUM over documents (§ 2.3)";

    // (4) and what it gathers against the LIVE tree is non-empty and a subset —
    // the two properties the engine's skip contract rests on. An empty scan is
    // not a failure to the engine, it is a silent skip, so nothing else would
    // notice this project's own `tests/features/` moving.
    QDir features(QString::fromUtf8(ANTS_SOURCE_DIR "/tests/features"));
    ASSERT_TRUE(features.exists());
    const QStringList dirs =
        features.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QSet<QString> existing(dirs.begin(), dirs.end());
    EXPECT_GT(existing.size(), 100) << "an empty scan skips the check silently";

    QFile cml(QString::fromUtf8(ANTS_SOURCE_DIR "/CMakeLists.txt"));
    ASSERT_TRUE(cml.open(QIODevice::ReadOnly));
    const QString cmake = QString::fromUtf8(cml.readAll());
    static const QRegularExpression re(
        QStringLiteral(R"(tests/features/([a-z0-9_]+)/test_[a-z0-9_]+\.cpp)"));
    QSet<QString> wired;
    auto it = re.globalMatch(cmake);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (existing.contains(name)) wired.insert(name);
    }
    EXPECT_FALSE(wired.isEmpty());
    EXPECT_TRUE(wired.contains(QStringLiteral("spec_lint")))
        << "this very test's directory must be in a bundle, or it is not running";
}
