// Feature-conformance test for ANTS-3716 — the `cited_by` sweep. Contract:
// tests/features/cited_by/spec.md; design: docs/specs/ANTS-3716-cited-by-sweep.md
//
// Behavioural through RemoteControl::cmdCitedBy against real fixture trees and
// a real ripgrep, because every invariant here is about what rg actually does —
// occurrences vs lines, event order across files, exit 2 on a missing path,
// double-counting an overlapped scope. A stubbed rg would assert the behaviour
// the author already believed. INV-9 and INV-10 are source scrapes: one asserts
// a refit, and the other's trigger cannot be provoked from a committable
// fixture (see the spec).

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// The store keys nothing here, but /tmp is a symlink on some hosts and the
// handler trims rg's absolute paths against the CANONICAL root — an
// uncanonicalised root would leave every `file` absolute and fail every cell
// assertion for the wrong reason.
QString canon(const QString &p) { return QFileInfo(p).canonicalFilePath(); }

QJsonObject run(const QJsonObject &req) {
    RemoteControl rc(nullptr);
    return rc.cmdCitedBy(req).object();
}

QJsonObject reqFor(const QString &root, const QStringList &anchors) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("anchors")]    = QJsonArray::fromStringList(anchors);
    return r;
}

QJsonObject cellAt(const QJsonObject &resp, int i) {
    return resp.value(QStringLiteral("cells")).toArray().at(i).toObject();
}

int cellCount(const QJsonObject &resp) {
    return resp.value(QStringLiteral("cells")).toArray().size();
}

QStringList strings(const QJsonObject &resp, const char *key) {
    QStringList out;
    for (const QJsonValue &v : resp.value(QLatin1String(key)).toArray())
        out << v.toString();
    return out;
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// `count` is OCCURRENCES, not matching lines. The twice-on-ONE-line anchor is
// the case that separates them: a per-line tally passes every other assertion
// here and fails only this one.
TEST(CitedBy, Inv1CellPerAnchorFilePairCountsOccurrences) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),
        "alpha and beta on one line\n"
        "gamma here\n"
        "gamma again on its own line\n"
        "delta delta twice on one line\n"));

    const QJsonObject resp = run(reqFor(
        root, {QStringLiteral("alpha"), QStringLiteral("beta"),
               QStringLiteral("gamma"), QStringLiteral("delta")}));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    ASSERT_EQ(cellCount(resp), 4);
    // Sorted by (anchor, file): alpha, beta, delta, gamma.
    EXPECT_EQ(cellAt(resp, 0).value("anchor").toString(), QStringLiteral("alpha"));
    EXPECT_EQ(cellAt(resp, 0).value("count").toInt(), 1);
    EXPECT_EQ(cellAt(resp, 1).value("anchor").toString(), QStringLiteral("beta"));
    EXPECT_EQ(cellAt(resp, 1).value("count").toInt(), 1);
    EXPECT_EQ(cellAt(resp, 2).value("anchor").toString(), QStringLiteral("delta"));
    EXPECT_EQ(cellAt(resp, 2).value("count").toInt(), 2)
        << "twice on ONE line must count 2 — this is occurrences, not lines";
    EXPECT_EQ(cellAt(resp, 3).value("anchor").toString(), QStringLiteral("gamma"));
    EXPECT_EQ(cellAt(resp, 3).value("count").toInt(), 2);
    // first_line is the LOWEST line for the pair.
    EXPECT_EQ(cellAt(resp, 3).value("first_line").toInt(), 2);
    EXPECT_EQ(cellAt(resp, 0).value("file").toString(), QStringLiteral("docs/a.md"));
}

// ---------------------------------------------------------------- INV-2 -----

// The two arrays partition `anchors` exactly, in request order — and are
// computed over every run, so they still hold when the cap truncated cells[].
TEST(CitedBy, Inv2MatchedUnmatchedPartitionSurvivesTruncation) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),
                          "alpha is cited here\n"));

    const QJsonObject resp = run(reqFor(
        root, {QStringLiteral("alpha"), QStringLiteral("nowhere")}));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(strings(resp, "anchors_matched"),   QStringList{QStringLiteral("alpha")});
    EXPECT_EQ(strings(resp, "anchors_unmatched"), QStringList{QStringLiteral("nowhere")});

    // Now a second anchor whose only citation sorts PAST a max_cells:1 cap.
    // "zulu" sorts after "alpha", so its cell is the one dropped — and it must
    // still be reported matched. A build that derived the arrays from the
    // capped cells would call it uncited.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/b.md"),
                          "zulu is cited here\n"));
    QJsonObject req = reqFor(root, {QStringLiteral("alpha"), QStringLiteral("zulu")});
    req[QStringLiteral("max_cells")] = 1;
    const QJsonObject capped = run(req);
    ASSERT_TRUE(capped.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(cellCount(capped), 1);
    EXPECT_TRUE(capped.value(QStringLiteral("truncated")).toBool());
    EXPECT_EQ(strings(capped, "anchors_matched"),
              (QStringList{QStringLiteral("alpha"), QStringLiteral("zulu")}))
        << "an anchor whose cell sorted past the cap must not read as uncited";
    EXPECT_TRUE(strings(capped, "anchors_unmatched").isEmpty());
}

// ---------------------------------------------------------------- INV-3 -----

// Anchors reach rg unescaped under --fixed-strings: a metacharacter matches
// itself, and a non-ASCII character does not abort the run. The accented case
// exits 2 with a regex parse error under any escaping layer, so it fails
// against the escaped design rather than passing trivially.
TEST(CitedBy, Inv3LiteralAndUnescaped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),
        "fooXcpp is not the anchor\n"
        "foo.cpp is the anchor\n"
        "the caf\xC3\xA9 anchor lives here\n"));

    const QJsonObject dotted = run(reqFor(root, {QStringLiteral("foo.cpp")}));
    ASSERT_TRUE(dotted.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(cellCount(dotted), 1);
    EXPECT_EQ(cellAt(dotted, 0).value("count").toInt(), 1)
        << "`.` must match itself, not any character";
    EXPECT_EQ(cellAt(dotted, 0).value("first_line").toInt(), 2);

    const QJsonObject accented =
        run(reqFor(root, {QString::fromUtf8("caf\xC3\xA9")}));
    EXPECT_TRUE(accented.value(QStringLiteral("ok")).toBool())
        << "an accented anchor must not abort its own rg run: "
        << QJsonDocument(accented).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(cellCount(accented), 1);
}

// ---------------------------------------------------------------- INV-4 -----

// `case` accepts insensitive (default) and sensitive and nothing else. "smart"
// refuses rather than being silently accepted: rg resolves --smart-case over
// the COMBINED pattern set, so one capitalised anchor would change every other
// anchor's result in the same request.
TEST(CitedBy, Inv4CaseModesAndNoSmart) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),
                          "the doc writes oldname in lower case\n"));

    const QJsonObject dflt = run(reqFor(root, {QStringLiteral("oldName")}));
    ASSERT_TRUE(dflt.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(cellCount(dflt), 1) << "insensitive is the default";

    QJsonObject sens = reqFor(root, {QStringLiteral("oldName")});
    sens[QStringLiteral("case")] = QStringLiteral("sensitive");
    const QJsonObject sensResp = run(sens);
    ASSERT_TRUE(sensResp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(cellCount(sensResp), 0);

    QJsonObject smart = reqFor(root, {QStringLiteral("oldName")});
    smart[QStringLiteral("case")] = QStringLiteral("smart");
    const QJsonObject smartResp = run(smart);
    EXPECT_FALSE(smartResp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(smartResp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
}

// ---------------------------------------------------------------- INV-5 -----

// An omitted scope resolves to the docs dir, README.md and CLAUDE.md — and
// nothing else.
TEST(CitedBy, Inv5DefaultScope) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),   "anchorX in docs\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/README.md"),   "anchorX in readme\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/CLAUDE.md"),   "unrelated\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/other/x.md"),  "anchorX out of scope\n"));

    const QJsonObject resp = run(reqFor(root, {QStringLiteral("anchorX")}));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    QStringList files;
    for (const QJsonValue &v : resp.value(QStringLiteral("cells")).toArray())
        files << v.toObject().value(QStringLiteral("file")).toString();
    files.sort();
    EXPECT_EQ(files, (QStringList{QStringLiteral("README.md"),
                                  QStringLiteral("docs/a.md")}));
    EXPECT_FALSE(files.contains(QStringLiteral("other/x.md")));
    EXPECT_EQ(resp.value(QStringLiteral("files_count")).toInt(), 2);
}

// ---------------------------------------------------------------- INV-6 -----

// Both refusals run no rg at all. Each pairs with a positive control, because
// a handler that refuses everything satisfies a refusal assertion alone.
TEST(CitedBy, Inv6ScopeEscapeAndAnchorArityRefuse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"), "anchorX here\n"));

    QJsonObject escaping = reqFor(root, {QStringLiteral("anchorX")});
    escaping[QStringLiteral("scope")] =
        QJsonArray::fromStringList({QStringLiteral("../outside")});
    const QJsonObject escResp = run(escaping);
    EXPECT_FALSE(escResp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(escResp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_path"));

    const QJsonObject empty = run(reqFor(root, {}));
    EXPECT_FALSE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(empty.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));

    // Positive control over the same fixture.
    EXPECT_TRUE(run(reqFor(root, {QStringLiteral("anchorX")}))
                    .value(QStringLiteral("ok")).toBool());
}

// ---------------------------------------------------------------- INV-7 -----

// cells is sorted by (anchor, file) and the cap is applied AFTER that sort, so
// two calls over an unchanged tree return byte-identical bodies.
//
// The single fixture file is deliberate: rg orders matches WITHIN a file by
// line but does not order across files at --threads 4, so a multi-file fixture
// would let an arrival-order build pass by luck.
TEST(CitedBy, Inv7SortedBeforeCapAndStable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"),
        "zeta appears first in the file\n"
        "alpha appears second in the file\n"));

    const QJsonObject resp = run(reqFor(
        root, {QStringLiteral("zeta"), QStringLiteral("alpha")}));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(cellCount(resp), 2);
    EXPECT_EQ(cellAt(resp, 0).value("anchor").toString(), QStringLiteral("alpha"))
        << "sorted by anchor, not by arrival or by request order";

    // The file dimension, under one anchor.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/b.md"), "alpha here too\n"));
    const QJsonObject twoFiles = run(reqFor(root, {QStringLiteral("alpha")}));
    ASSERT_EQ(cellCount(twoFiles), 2);
    EXPECT_EQ(cellAt(twoFiles, 0).value("file").toString(),
              QStringLiteral("docs/a.md"));
    EXPECT_EQ(cellAt(twoFiles, 1).value("file").toString(),
              QStringLiteral("docs/b.md"));

    // Byte-identical under truncation.
    QJsonObject capped = reqFor(root, {QStringLiteral("alpha")});
    capped[QStringLiteral("max_cells")] = 1;
    const QByteArray first  = QJsonDocument(run(capped)).toJson(QJsonDocument::Compact);
    const QByteArray second = QJsonDocument(run(capped)).toJson(QJsonDocument::Compact);
    EXPECT_EQ(first, second);
}

// ---------------------------------------------------------------- INV-8 -----

// cells_count is the CAPPED length; files_count is uncapped. The files_count
// assertion is the one that fails the natural build, which derives it from the
// capped array — a cap that reads as completeness is the defect this names.
TEST(CitedBy, Inv8CappedCellsUncappedFilesCount) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    // 5 cells over 4 distinct files: aa cites both anchors, the rest one each.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/aa.md"), "alpha and bravo\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/bb.md"), "alpha only\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/cc.md"), "alpha only\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/dd.md"), "bravo only\n"));

    QJsonObject req = reqFor(root, {QStringLiteral("alpha"), QStringLiteral("bravo")});
    req[QStringLiteral("max_cells")] = 2;
    const QJsonObject resp = run(req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(cellCount(resp), 2);
    EXPECT_EQ(resp.value(QStringLiteral("cells_count")).toInt(), 2);
    EXPECT_TRUE(resp.value(QStringLiteral("truncated")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("files_count")).toInt(), 4)
        << "files_count must be the UNCAPPED distinct-file total";
}

// ---------------------------------------------------------------- INV-9 -----

// One rg call site, in a helper that runs the process to completion and
// classifies nothing. Source scrape: it asserts a refit, so it cannot fail
// before the extraction lands.
TEST(CitedBy, Inv9OneRgCallSiteAndNoProcessInHandlers) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(ants_test::countOccurrences(rc, "rg.start(") == 1,
           "INV-9: exactly one rg.start( call site across the RemoteControl TUs");

    const std::string ws =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdWorkspaceSearch");
    const std::string cb =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdCitedBy");
    expect(!ws.empty(), "INV-9: cmdWorkspaceSearch body found");
    expect(!cb.empty(), "INV-9: cmdCitedBy body found");
    expect(ws.find("QProcess") == std::string::npos,
           "INV-9: cmdWorkspaceSearch body names QProcess");
    expect(cb.find("QProcess") == std::string::npos,
           "INV-9: cmdCitedBy body names QProcess");

    // The helper returns the raw run and builds no refusal envelope, so the
    // classification cannot silently migrate into it.
    const std::string rgRun = ants_test::slurpFunctionBody(rc, "struct RgRun");
    expect(!rgRun.empty(), "INV-9: RgRun struct found");
    expect(rgRun.find("QJsonObject") == std::string::npos,
           "INV-9: RgRun carries a refusal envelope");
    EXPECT_EQ(0, expect_failures());
}

// --------------------------------------------------------------- INV-10 -----

// Any failed rg run refuses; cited_by never returns ok:true with a partial cell
// set. A source scrape by necessity — timeout_sec floors at 1 s and rg searches
// hundreds of MB in well under that, so a live trigger would need a
// several-hundred-MB fixture in a `features;fast` bundle and would still be
// load-dependent on a 4-vCPU CI host. That is a flake, not a test. Follows the
// precedent of tests/features/mcp_workspace_search_timeout_sec/ INV-3 / INV-4.
TEST(CitedBy, Inv10FailedRunRefusesWithNoPartialGuard) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string cb =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdCitedBy");
    expect(!cb.empty(), "INV-10: cmdCitedBy body found");
    // COMMENTS ARE STRIPPED FIRST: the handler's own comment names the guard it
    // deliberately drops, and this assertion is about the code, not the prose
    // explaining it.
    const std::string cbCode = ants_test::stripComments(cb);
    expect(cbCode.find("matches.isEmpty()") == std::string::npos,
           "INV-10: cmdCitedBy carries a partial-results guard on a failure "
           "branch — a partial cell set is indistinguishable from a complete "
           "one to the caller");
    expect(cb.find("\"rg_failed\"") != std::string::npos,
           "INV-10: cmdCitedBy raises rg_failed");
    expect(cb.find("cited_by: rg") != std::string::npos,
           "INV-10: cmdCitedBy's rg_failed message names this verb, not "
           "workspace-search");
    EXPECT_EQ(0, expect_failures());
}

// --------------------------------------------------------------- INV-11 -----

// A scope entry absent on disk is pruned before argv is built. rg exits 2 on a
// path that is not there, and the default scope names two files not every
// project has — left unhandled, the DEFAULT call would refuse on such a project.
TEST(CitedBy, Inv11MissingScopeEntriesArePruned) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"), "anchorX here\n"));
    // Deliberately no CLAUDE.md and no README.md.

    const QJsonObject resp = run(reqFor(root, {QStringLiteral("anchorX")}));
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a missing default-scope entry must not become rg_failed: "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(strings(resp, "scope_resolved"), QStringList{QStringLiteral("docs")});
    EXPECT_EQ(cellCount(resp), 1);

    QJsonObject nope = reqFor(root, {QStringLiteral("anchorX")});
    nope[QStringLiteral("scope")] =
        QJsonArray::fromStringList({QStringLiteral("nope")});
    const QJsonObject nopeResp = run(nope);
    EXPECT_TRUE(nopeResp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(strings(nopeResp, "scope_resolved").isEmpty());
    EXPECT_EQ(cellCount(nopeResp), 0);
    EXPECT_EQ(strings(nopeResp, "anchors_unmatched"),
              QStringList{QStringLiteral("anchorX")});
}

// --------------------------------------------------------------- INV-12 -----

// Surviving scope entries are de-overlapped. rg emits a match event per
// positional path that reaches a file, so scope:["docs","docs/sub"] would
// DOUBLE that pair's count — and no other case here uses an overlapping scope,
// so the naive build's doubled count would go unnoticed.
TEST(CitedBy, Inv12OverlappingScopeIsDeOverlapped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/sub/deep.md"),
                          "anchorX cited once\n"));

    QJsonObject req = reqFor(root, {QStringLiteral("anchorX")});
    req[QStringLiteral("scope")] = QJsonArray::fromStringList(
        {QStringLiteral("docs"), QStringLiteral("docs/sub")});
    const QJsonObject resp = run(req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(cellCount(resp), 1);
    EXPECT_EQ(cellAt(resp, 0).value("count").toInt(), 1)
        << "an overlapped scope must not double a cell's occurrence count";
    EXPECT_EQ(strings(resp, "scope_resolved"), QStringList{QStringLiteral("docs")});
    EXPECT_EQ(resp.value(QStringLiteral("files_count")).toInt(), 1);
}

// --------------------------------------------------------------- INV-13 -----

// An empty-string anchor refuses before any rg run: `rg -F -e ''` matches at
// every byte position, so one of them saturates the collection ceiling with
// junk and starves every real anchor of the cap. Paired with a positive control
// over the same fixture.
TEST(CitedBy, Inv13EmptyAnchorRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"), "oldName here\n"));

    const QJsonObject bad =
        run(reqFor(root, {QString(), QStringLiteral("oldName")}));
    EXPECT_FALSE(bad.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));

    const QJsonObject good = run(reqFor(root, {QStringLiteral("oldName")}));
    EXPECT_TRUE(good.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(cellCount(good), 1);
}
