// ANTS-3663 — doc_lint VERB conformance test. Phase-1 rows: INV-2, INV-8,
// INV-11, INV-13. `RemoteControl::cmdDocLint` itself needs a live MainWindow, so
// behavioural rows drive the pure helper (and the engine, for the half INV-2
// asserts about narrowing) while wiring rows source-scrape the registration
// sites — the pattern ANTS-3601's, ANTS-3661's and ANTS-3660's verb tests use.

#include "remotecontrol.h"
#include "doclint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
#error "doc_lint_verb test needs the test_claude source-path compile defs"
#endif

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// A throwaway project root. Canonical for the reason doc_citations' shared
// fixture states: /tmp is a symlink on some distributions, and an
// uncanonicalised root makes every in-root citation look like an escape.
struct Tree {
    QTemporaryDir dir;
    QString root;
    Tree() : root(QFileInfo(dir.path()).canonicalFilePath()) {}
    void write(const QString &rel, const QByteArray &body) const {
        const QString abs = root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(abs).path());
        QFile f(abs);
        if (f.open(QIODevice::WriteOnly)) { f.write(body); f.close(); }
    }
};

DocLint::Options optsFor(const Tree &t) {
    DocLint::Options o;
    o.rootCanonical         = t.root;
    o.symbols.rootCanonical = t.root;
    o.specsDirRel           = QStringLiteral("docs/specs");
    return o;
}

int countVerb(const DocLint::Result &r, const QString &verb) {
    int n = 0;
    for (const DocFinding::Finding &f : r.findings) if (f.verb == verb) ++n;
    return n;
}

}  // namespace

// INV-2 — checks[] narrows findings AND counts together; an unknown name
// refuses bad_args.
//
// GUARDED BY AN UNFILTERED RUN FIRST. Without it the narrowing half passes
// against an engine that produces nothing at all, which is the state it first
// runs in.
TEST(DocLintVerb, Inv2CheckFilterAndUnknownName) {
    Tree t;
    const QString doc = QStringLiteral("docs/a.md");
    t.write(doc, "# A\n\n[dead](nowhere.md)\n\nsee `src/gone.cpp:1` here\n");

    // Guard: BOTH checkers really do produce findings on this fixture.
    const DocLint::Result all = DocLint::run({doc}, optsFor(t));
    ASSERT_GE(countVerb(all, QStringLiteral("doc_integrity")), 1)
        << "fixture must produce doc_integrity findings, or the row asserts nothing";
    ASSERT_GE(countVerb(all, QStringLiteral("doc_citations")), 1)
        << "fixture must produce doc_citations findings, or the row asserts nothing";

    DocLint::Options narrowed = optsFor(t);
    narrowed.checks = {QStringLiteral("doc_integrity")};
    const DocLint::Result one = DocLint::run({doc}, narrowed);

    EXPECT_GE(countVerb(one, QStringLiteral("doc_integrity")), 1);
    EXPECT_EQ(0, countVerb(one, QStringLiteral("doc_citations")));

    // ...and `counts` narrows WITH the findings, not independently of them.
    const QJsonObject o = RemoteControl::docLintBuildResponse(one, 500);
    const QJsonObject counts = o.value(QStringLiteral("counts")).toObject();
    EXPECT_TRUE(counts.contains(QStringLiteral("doc_integrity")));
    EXPECT_FALSE(counts.contains(QStringLiteral("doc_citations")));
    EXPECT_FALSE(o.value(QStringLiteral("checks_run")).toArray()
                     .contains(QJsonValue(QStringLiteral("doc_citations"))));

    // The refusal arm. A filter that silently widened would return the report
    // the caller passed it to avoid, and say nothing about why.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const QString handler = QString::fromStdString(
        ants_test::slurpFunctionBody(rc.toStdString(), "RemoteControl::cmdDocLint"));
    ASSERT_FALSE(handler.isEmpty()) << "cmdDocLint body not found";
    EXPECT_TRUE(handler.contains(QStringLiteral("bad_args")));
    EXPECT_TRUE(handler.contains(QStringLiteral("accepted")));
    // The GUARD, not merely the identifier. `checkNames()` also appears in the
    // `accepted` list this refusal emits, so asserting the bare name passes
    // against a handler whose membership test has been deleted — measured: that
    // mutation survived until this assertion replaced it.
    EXPECT_TRUE(handler.contains(QStringLiteral("!DocLint::checkNames().contains(")))
        << "the unknown-name refusal must be GATED on membership, not just mention it";
}

// INV-8 — the verb-contract minimum mcp-tools.md § Tests requires of every
// Required tool, in the arms ANTS-3601 INV-10 and INV-15 establish.
TEST(DocLintVerb, Inv8VerbContractMinimums) {
    // (1) caller_cwd Required — at the call site AND in the static table.
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    ASSERT_GE(mw.indexOf(QStringLiteral("registerToolProvider(\"doc_lint\"")), 0);
    EXPECT_TRUE(QString::fromStdString(ants_test::regionBetween(
                    mw.toStdString(), "registerToolProvider(\"doc_lint\"",
                    "registerToolProvider("))
                    .contains(QStringLiteral("CallerCwdContract::Required")));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"doc_lint\")"));
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains(QStringLiteral("C::Required")));
    EXPECT_TRUE(ci.contains(QStringLiteral("docLint[\"name\"] = \"doc_lint\"")));

    // (2) a supplied path is validated BEFORE any enumeration → bad_path.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    const QString handler = QString::fromStdString(
        ants_test::slurpFunctionBody(rc.toStdString(), "RemoteControl::cmdDocLint"));
    ASSERT_FALSE(handler.isEmpty()) << "cmdDocLint body not found";
    EXPECT_TRUE(handler.contains(QStringLiteral("validatePath(")));
    EXPECT_TRUE(handler.contains(QStringLiteral("check.err")));
    const int validate  = handler.indexOf(QStringLiteral("validatePath("));
    const int enumerate = handler.indexOf(QStringLiteral("docIntegrityEnumerate("));
    ASSERT_GE(enumerate, 0);
    EXPECT_LT(validate, enumerate)
        << "the path must be validated before the walk, not after it";

    // (3) THE ARM THAT IS NOT A REFUSAL. A well-formed non-existent in-root path
    // yields ok:true with an empty checked_docs, because docIntegrityEnumerate
    // returns {} for one. An implementer who reads "the refusal minimums" and
    // builds three refusals breaks a contract the whole family shares.
    const QJsonObject empty = RemoteControl::docLintBuildResponse({}, 500);
    EXPECT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(empty.value(QStringLiteral("findings")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("checked_docs")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("checks_run")).toArray().isEmpty());
    EXPECT_FALSE(empty.contains(QStringLiteral("truncated")));
    // Omit-when-empty keys stay absent rather than arriving empty.
    for (const char *k : {"skipped", "check_errors", "pairs", "clusters", "check_stats"})
        EXPECT_FALSE(empty.contains(QString::fromUtf8(k)));
    // Phase 1 has no fix path, so the three fix-only keys are absent entirely —
    // files_written:0 on a report-only run would read as "tried and wrote
    // nothing" when the truth is "never entered the fix path".
    for (const char *k : {"fixed", "files_written", "fix_errors", "dry_run"})
        EXPECT_FALSE(empty.contains(QString::fromUtf8(k)));
}

// INV-11 — max_findings truncates AFTER the sort, and counts is computed before
// it. The PREFIX IDENTITY is the assertion: a cap applied during collection also
// returns five findings and also sets the flag, and only the comparison against
// the uncapped run tells the two apart.
TEST(DocLintVerb, Inv11CapTruncatesAfterTheSort) {
    DocLint::Result r;
    for (int i = 0; i < 12; ++i) {
        DocFinding::Finding f;
        f.verb = QStringLiteral("doc_integrity");
        f.kind = QStringLiteral("broken_link");
        f.file = QStringLiteral("docs/%1.md").arg(i, 2, 10, QLatin1Char('0'));
        f.line = 1 + i;
        f.message = QStringLiteral("target %1 not found").arg(i);
        f.emissionIndex = i;
        r.findings.append(f);
    }
    r.checkedDocs << QStringLiteral("docs/00.md");
    r.checksRun   << QStringLiteral("doc_integrity");

    const QJsonArray full =
        RemoteControl::docLintBuildResponse(r, 500).value(QStringLiteral("findings")).toArray();
    const QJsonObject capped = RemoteControl::docLintBuildResponse(r, 5);
    const QJsonArray page = capped.value(QStringLiteral("findings")).toArray();

    ASSERT_EQ(12, full.size());
    ASSERT_EQ(5, page.size());
    for (int i = 0; i < page.size(); ++i)
        EXPECT_EQ(full.at(i).toObject(), page.at(i).toObject())
            << "the page must be a PREFIX of the uncapped run";
    EXPECT_TRUE(capped.value(QStringLiteral("truncated")).toBool());

    // counts describes the whole run, not the page.
    const QJsonObject counts = capped.value(QStringLiteral("counts")).toObject()
                                   .value(QStringLiteral("doc_integrity")).toObject();
    EXPECT_EQ(12, counts.value(QStringLiteral("broken_link")).toInt());

    // An uncapped run carries no flag at all.
    EXPECT_FALSE(RemoteControl::docLintBuildResponse(r, 500)
                     .contains(QStringLiteral("truncated")));

    // The bounds are clamped, never refused: 0 -> 1 and 9999 -> 5000.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    const QString handler = QString::fromStdString(
        ants_test::slurpFunctionBody(rc.toStdString(), "RemoteControl::cmdDocLint"));
    ASSERT_FALSE(handler.isEmpty());
    EXPECT_TRUE(handler.contains(QStringLiteral("qBound(1,")));
    EXPECT_TRUE(handler.contains(QStringLiteral("5000")));
    EXPECT_TRUE(handler.contains(QStringLiteral("toInt(500)")));
}

// INV-13 — no doc_lint call can short-circuit to {ok, unchanged}. A REGISTRATION
// assertion by necessity: a verb outside the ETag set returns no etag, so there
// is nothing to feed back, and an arbitrary etag_match would never match even
// for a verb that IS in the set. This is the row that fails against the obvious
// registration, which is to add doc_lint alongside its five siblings.
TEST(DocLintVerb, Inv13EtagNeverSkipsAFix) {
    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());

    // (a) the registered schema carries no etag_match property.
    const QString descriptor = QString::fromStdString(ants_test::regionBetween(
        ci.toStdString(), "docLint[\"name\"] = \"doc_lint\"", "tools.append(docLint);"));
    ASSERT_FALSE(descriptor.isEmpty()) << "doc_lint tools/list entry not found";
    EXPECT_FALSE(descriptor.contains(QStringLiteral("props[\"etag_match\"]")))
        << "doc_lint must not advertise a short-circuit it never performs";

    // (b) isEtagSupportedTool has no doc_lint entry.
    const QString fn = QString::fromStdString(ants_test::slurpFunctionBody(
        ci.toStdString(), "ClaudeIntegration::isEtagSupportedTool"));
    ASSERT_FALSE(fn.isEmpty()) << "isEtagSupportedTool body not found";
    EXPECT_FALSE(fn.contains(QStringLiteral("\"doc_lint\"")))
        << "an etag_match that matched would report a COMPLETED repair as unchanged";
}
