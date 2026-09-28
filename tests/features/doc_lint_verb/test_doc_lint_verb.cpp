// ANTS-3663 — doc_lint VERB conformance test. Phase-1 rows: INV-2, INV-8,
// INV-11, INV-13. `RemoteControl::cmdDocLint` itself needs a live MainWindow, so
// behavioural rows drive the pure helper (and the engine, for the half INV-2
// asserts about narrowing) while wiring rows source-scrape the registration
// sites — the pattern ANTS-3601's, ANTS-3661's and ANTS-3660's verb tests use.
//
// ANTS-5506 adds two doc_facts verb-layer rows: INV-26 (version truth prefers
// `.claude/bump.json` over `CMakeLists.txt`) and INV-28 (the live tools/list
// schema is forwarded into verb_arg_unknown's map). Both need `cmdDocLint`'s
// two injected inputs (§ 2.6), which only a real MainWindow (or, here,
// ants-mcpd, which wires the same provider in src/mcpdmain.cpp) supplies — so
// both drive a real ants-mcpd child process via ants_test::McpdSession, the
// same harness tests/features/mcpd_call/ uses. THE CODE DOES NOT EXIST YET at
// the time these rows were written: DocLint::Options carries no
// projectVersion or verbArgs member, and "doc_facts" is not in
// DocLint::checkNames() — see the ANTS-3663 bug description for the exact
// interface these rows are written against.

#include "remotecontrol.h"
#include "doclint.h"

#include "../standalone_mcp_server/mcpd_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#if !defined(ANTS_MAINWINDOW_SOURCES) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
#error "doc_lint_verb test needs the test_claude source-path compile defs"
#endif
#ifndef ANTS_MCPD_BIN
#error "doc_lint_verb test needs ANTS_MCPD_BIN for the INV-26/INV-28 ants-mcpd cases"
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
    const QString mw = QString::fromStdString(ants_test::slurpMainWindow());
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

namespace {

// A version_drift finding count, read out of the wire envelope.
int wireVersionDriftFindings(const QJsonObject &r) {
    int n = 0;
    for (const auto &v : r.value(QStringLiteral("findings")).toArray())
        if (v.toObject().value(QStringLiteral("kind")).toString() ==
            QStringLiteral("version_drift"))
            ++n;
    return n;
}

// § 2.6 — version_unavailable is always PRESENT and boolean whenever
// doc_facts ran, never omitted. Returning the QJsonValue (not toBool())
// lets the caller tell "false" from "absent": QJsonValue().toBool() is also
// false, so a bare toBool() comparison passes vacuously against a key the
// implementation never emits.
QJsonValue wireDocFactsVersionUnavailable(const QJsonObject &r) {
    return r.value(QStringLiteral("check_stats")).toObject()
        .value(QStringLiteral("doc_facts")).toObject()
        .value(QStringLiteral("version_unavailable"));
}

// Writes CMakeLists.txt (VERSION 1.0.0), a document claiming that same
// version, and — when `withBumpFile` — a `.claude/bump.json` whose
// version_source/version_pattern resolve to 2.0.0. Returns the project's
// canonical root, or an empty string on failure.
QString writeVersionTruthFixture(const QTemporaryDir &tmp, const QString &name,
                                 bool withBumpFile) {
    const QString proj = tmp.filePath(name);
    if (!QDir().mkpath(proj + QStringLiteral("/docs"))) return {};
    if (withBumpFile) {
        if (!QDir().mkpath(proj + QStringLiteral("/.claude"))) return {};
        QFile bump(proj + QStringLiteral("/.claude/bump.json"));
        if (!bump.open(QIODevice::WriteOnly)) return {};
        const QByteArray bumpBody = QByteArray(
            "{\n"
            "  \"version_source\": \"docs/VERSION_TRUTH.txt\",\n"
            "  \"version_pattern\": \"([0-9]+\\\\.[0-9]+\\\\.[0-9]+)\"\n"
            "}\n");
        if (bump.write(bumpBody) != bumpBody.size()) return {};
        QFile truth(proj + QStringLiteral("/docs/VERSION_TRUTH.txt"));
        if (!truth.open(QIODevice::WriteOnly)) return {};
        const QByteArray truthBody = "2.0.0\n";
        if (truth.write(truthBody) != truthBody.size()) return {};
    }
    QFile cmake(proj + QStringLiteral("/CMakeLists.txt"));
    if (!cmake.open(QIODevice::WriteOnly)) return {};
    const QByteArray cmakeBody = "project(x VERSION 1.0.0)\n";
    if (cmake.write(cmakeBody) != cmakeBody.size()) return {};
    QFile claim(proj + QStringLiteral("/docs/claim.md"));
    if (!claim.open(QIODevice::WriteOnly)) return {};
    const QByteArray claimBody =
        "# Claim\n\nVersion <strong>1.0.0</strong> is what this document says.\n";
    if (claim.write(claimBody) != claimBody.size()) return {};
    return QFileInfo(proj).canonicalFilePath();
}

}  // namespace

// ANTS-5506 INV-26 — cmdDocLint resolves the version from .claude/bump.json
// BEFORE CMakeLists.txt. With the bump file present (pointing at 2.0.0) the
// document's "Version <strong>1.0.0" claim mismatches -> one version_drift
// finding. Without the bump file, the fallback (CMakeLists.txt's own 1.0.0)
// matches the same claim -> none. version_unavailable is false in both runs:
// some source always resolves.
TEST(DocLintVerb, Inv26VersionTruthPrefersBumpFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString withBump = writeVersionTruthFixture(tmp, QStringLiteral("inv26-with"), true);
    const QString withoutBump =
        writeVersionTruthFixture(tmp, QStringLiteral("inv26-without"), false);
    ASSERT_FALSE(withBump.isEmpty());
    ASSERT_FALSE(withoutBump.isEmpty());

    ants_test::McpdSession withSession(withBump, tmp.filePath(QStringLiteral("with.sock")));
    ASSERT_TRUE(withSession.started());
    const QJsonObject withResult = withSession.call(QStringLiteral("doc_lint"),
        QJsonObject{{"caller_cwd", withBump}, {"checks", QJsonArray{QStringLiteral("doc_facts")}}});
    ASSERT_FALSE(withResult.value(QStringLiteral("test_timeout")).toBool())
        << "doc_lint call (with bump.json) timed out";
    EXPECT_EQ(1, wireVersionDriftFindings(withResult))
        << "bump.json points at 2.0.0, the document claims 1.0.0 — a mismatch: "
        << QJsonDocument(withResult).toJson().toStdString();
    EXPECT_TRUE(withResult.value(QStringLiteral("checks_run")).toArray()
                    .contains(QJsonValue(QStringLiteral("doc_facts"))))
        << "doc_facts must actually have run, or the zero/one-finding "
           "comparison below is vacuous: "
        << QJsonDocument(withResult).toJson().toStdString();
    const QJsonValue withUnavailable = wireDocFactsVersionUnavailable(withResult);
    ASSERT_TRUE(withUnavailable.isBool())
        << "version_unavailable must be present and boolean whenever doc_facts "
           "ran (§ 2.6), not merely absent: "
        << QJsonDocument(withResult).toJson().toStdString();
    EXPECT_FALSE(withUnavailable.toBool())
        << QJsonDocument(withResult).toJson().toStdString();

    ants_test::McpdSession withoutSession(withoutBump, tmp.filePath(QStringLiteral("without.sock")));
    ASSERT_TRUE(withoutSession.started());
    const QJsonObject withoutResult = withoutSession.call(QStringLiteral("doc_lint"),
        QJsonObject{{"caller_cwd", withoutBump}, {"checks", QJsonArray{QStringLiteral("doc_facts")}}});
    ASSERT_FALSE(withoutResult.value(QStringLiteral("test_timeout")).toBool())
        << "doc_lint call (without bump.json) timed out";
    EXPECT_EQ(0, wireVersionDriftFindings(withoutResult))
        << "no bump.json: falls back to CMakeLists.txt's 1.0.0, matching the "
           "document's claim — no drift: "
        << QJsonDocument(withoutResult).toJson().toStdString();
    EXPECT_TRUE(withoutResult.value(QStringLiteral("checks_run")).toArray()
                    .contains(QJsonValue(QStringLiteral("doc_facts"))))
        << "doc_facts must actually have run, or the zero-finding result "
           "above is vacuous (it never ran at all): "
        << QJsonDocument(withoutResult).toJson().toStdString();
    const QJsonValue withoutUnavailable = wireDocFactsVersionUnavailable(withoutResult);
    ASSERT_TRUE(withoutUnavailable.isBool())
        << "version_unavailable must be present and boolean whenever doc_facts "
           "ran (§ 2.6), not merely absent: "
        << QJsonDocument(withoutResult).toJson().toStdString();
    EXPECT_FALSE(withoutUnavailable.toBool())
        << QJsonDocument(withoutResult).toJson().toStdString();
}

// ANTS-5506 INV-28 — the verb layer forwards the LIVE tools/list schema. An
// ants-mcpd session that has served tools/list before calling doc_lint sees
// two verb_arg_unknown findings over three doc_lint call spans: a typo'd
// "pathz" key, a clean {encoding, path} call, and an "etag_match" key doc_lint
// does not honour (§ 2.5 — doc_lint is outside isEtagSupportedTool, INV-13).
// schema_unavailable is false because tools/list was served first.
TEST(DocLintVerb, Inv28LiveSchemaIsForwarded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString proj = tmp.filePath(QStringLiteral("inv28"));
    ASSERT_TRUE(QDir().mkpath(proj + QStringLiteral("/docs")));
    QFile f(proj + QStringLiteral("/docs/calls.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    const QByteArray body =
        "# Calls\n"
        "\n"
        "`doc_lint {pathz:\"docs\"}`\n"
        "\n"
        "`doc_lint {encoding:\"tabular\", path:\"docs\"}`\n"
        "\n"
        "`doc_lint {etag_match:\"e\"}`\n";
    ASSERT_EQ(f.write(body), body.size());
    f.close();
    const QString root = QFileInfo(proj).canonicalFilePath();

    ants_test::McpdSession session(root, tmp.filePath(QStringLiteral("inv28.sock")));
    ASSERT_TRUE(session.started());

    // Serve tools/list FIRST — before it, the vocabulary provider's published
    // copy is empty and verb_arg_unknown does not run at all (§ 2.6).
    const int listId = session.send(QStringLiteral("tools/list"));
    const QJsonObject listReply = session.await(listId);
    ASSERT_FALSE(listReply.value(QStringLiteral("test_timeout")).toBool())
        << "tools/list timed out";

    const QJsonObject r = session.call(QStringLiteral("doc_lint"),
        QJsonObject{{"caller_cwd", root}, {"checks", QJsonArray{QStringLiteral("doc_facts")}}});
    ASSERT_FALSE(r.value(QStringLiteral("test_timeout")).toBool())
        << "doc_lint call timed out";

    QStringList unknownArgMessages;
    for (const auto &v : r.value(QStringLiteral("findings")).toArray()) {
        const QJsonObject fo = v.toObject();
        if (fo.value(QStringLiteral("kind")).toString() ==
            QStringLiteral("verb_arg_unknown"))
            unknownArgMessages << fo.value(QStringLiteral("message")).toString();
    }
    ASSERT_EQ(2, unknownArgMessages.size())
        << QJsonDocument(r).toJson().toStdString();
    bool sawPathz = false, sawEtagMatch = false;
    for (const QString &m : unknownArgMessages) {
        if (m.contains(QStringLiteral("pathz"))) sawPathz = true;
        if (m.contains(QStringLiteral("etag_match"))) sawEtagMatch = true;
    }
    EXPECT_TRUE(sawPathz) << QJsonDocument(r).toJson().toStdString();
    EXPECT_TRUE(sawEtagMatch)
        << "doc_lint does not honour ETags (§ 2.5) — etag_match must be "
           "flagged as an unknown top-level key: "
        << QJsonDocument(r).toJson().toStdString();

    // § 2.6 — schema_unavailable must be PRESENT and boolean whenever
    // doc_facts ran, never merely absent: QJsonValue().toBool() is also
    // false, so a bare toBool() comparison passes vacuously against a key
    // the implementation never emits.
    const QJsonValue schemaUnavailable = r.value(QStringLiteral("check_stats")).toObject()
        .value(QStringLiteral("doc_facts")).toObject()
        .value(QStringLiteral("schema_unavailable"));
    ASSERT_TRUE(schemaUnavailable.isBool())
        << "schema_unavailable must be present and boolean whenever doc_facts ran: "
        << QJsonDocument(r).toJson().toStdString();
    EXPECT_FALSE(schemaUnavailable.toBool())
        << "tools/list was served before this call: " << QJsonDocument(r).toJson().toStdString();
}
