// ANTS-3669 — doc_lint FIX-path conformance test. Phase-2 rows: INV-5, INV-6,
// INV-12, INV-14, INV-15, INV-16, INV-18, INV-21.
//
// This is the only part of the doc-lint family that writes to a file, so most
// rows here assert an ABSENCE — nothing written, nothing appended, a key not
// present. Every one of those passes vacuously against an engine that repairs
// nothing, which is the state they first ran in, so each is re-proven by
// deleting the rule under test and confirming the fixture still fails. The
// mutation table is in this directory's spec.md, including the ones that
// reddened nothing.
//
// `RemoteControl::cmdDocLint` needs a live MainWindow, so behavioural rows drive
// the engine and the pure response builder while the request-argument row
// source-scrapes its guard — the pattern the sibling doc_lint_verb test uses.

#include "remotecontrol.h"
#include "doclint.h"

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

#if !defined(ANTS_RC_SOURCES)
#error "doc_lint_fix test needs the test_claude source-path compile defs"
#endif

namespace {

QString slurp(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// A throwaway project root. Canonical for the reason doc_citations' shared
// fixture states: /tmp is a symlink on some distributions, and an
// uncanonicalised root makes every in-root path look like an escape.
struct Tree {
    QTemporaryDir dir;
    QString       root;
    Tree() : root(QFileInfo(dir.path()).canonicalFilePath()) {}

    QString abs(const QString &rel) const { return root + QLatin1Char('/') + rel; }

    void write(const QString &rel, const QString &body) const {
        const QString a = abs(rel);
        QDir().mkpath(QFileInfo(a).path());
        QFile f(a);
        if (f.open(QIODevice::WriteOnly)) { f.write(body.toUtf8()); f.close(); }
    }

    QString read(const QString &rel) const { return slurp(abs(rel)); }
};

DocLint::Options optsFor(const Tree &t, bool fix = false, bool dryRun = false) {
    DocLint::Options o;
    o.rootCanonical         = t.root;
    o.symbols.rootCanonical = t.root;
    o.specsDirRel           = QStringLiteral("docs/specs");
    o.fix                   = fix;
    o.dryRun                = dryRun;
    return o;
}

// Failure detail: the whole envelope, so a red line says what actually came
// back rather than only which predicate failed.
std::string render(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

std::string renderResult(const DocLint::Result &r) {
    QJsonObject o;
    o[QStringLiteral("findings")] = DocFinding::toJson(r.findings);
    o[QStringLiteral("fixed")]    = DocFinding::toJson(r.fixed);
    o[QStringLiteral("files_written")] = r.filesWritten;
    QJsonArray errs;
    for (const DocLint::FixError &e : r.fixErrors)
        errs.append(QJsonObject{{QStringLiteral("file"), e.file},
                                {QStringLiteral("reason"), e.reason}});
    o[QStringLiteral("fix_errors")] = errs;
    return render(o);
}

QStringList reasonsOf(const DocLint::Result &r) {
    QStringList out;
    for (const DocLint::FixError &e : r.fixErrors) out << e.reason;
    return out;
}

// --- fixtures -------------------------------------------------------------

// One missing H2. `## Contents` itself is exempt because its line sits at or
// above the TOC run, which is DocIntegrity's own rule.
const QString kSimple = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Alpha](#alpha)\n"
    "- [Gamma](#gamma)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "a\n"
    "\n"
    "## Beta\n"
    "\n"
    "b\n"
    "\n"
    "## Gamma\n"
    "\n"
    "g\n");

// Three gaps, ONE document — the pair fixed[] and files_written exist to keep
// apart.
const QString kThreeGaps = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Alpha](#alpha)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "## Beta\n"
    "\n"
    "## Gamma\n"
    "\n"
    "## Delta\n");

// INV-18's discriminator. The TOC carries all three row types a regeneration
// from the H2 set would eat: an H3 child entry, a free-text row, and an entry
// whose slug matches no heading. A fixture without all three cannot tell a
// patch from a regeneration.
const QString kRich = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Alpha](#alpha)\n"
    "  - [Sub thing](#sub-thing)\n"
    "- see also the appendix\n"
    "- [Ghost](#ghost)\n"
    "- [Gamma](#gamma)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "### Sub thing\n"
    "\n"
    "## Beta\n"
    "\n"
    "## Gamma\n");

// The other toc_gap cause. The repair deletes the LATER occurrence, which
// DocIntegrity reports; the first is kept.
const QString kDuplicate = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Alpha](#alpha)\n"
    "- [Alpha](#alpha)\n"
    "- [Beta](#beta)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "## Beta\n");

// The same document with the duplicate already gone — what a user's save looks
// like. The line the walk reported now holds a DIFFERENT, innocent entry.
const QString kDuplicateFixed = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Alpha](#alpha)\n"
    "- [Beta](#beta)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "## Beta\n");

// The only linked entry points at an H3, so there is no top-level row whose
// indent and marker an inserted one could copy.
const QString kNoTemplate = QStringLiteral(
    "# Doc\n"
    "\n"
    "## Contents\n"
    "\n"
    "- [Sub](#sub)\n"
    "\n"
    "## Alpha\n"
    "\n"
    "### Sub\n"
    "\n"
    "## Beta\n");

}  // namespace

// ---------------------------------------------------------------------------
// INV-6 — report-only by default.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv6ReportOnlyByDefault) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kSimple);

    const DocLint::Result r = DocLint::run({QStringLiteral("docs/a.md")}, optsFor(t));

    // The gap is FOUND — otherwise this row would pass for the wrong reason,
    // reporting restraint about a document with nothing to repair.
    ASSERT_FALSE(r.findings.isEmpty()) << renderResult(r);
    EXPECT_EQ(kSimple, t.read(QStringLiteral("docs/a.md"))) << "default run wrote to disk";
    EXPECT_TRUE(r.fixed.isEmpty()) << renderResult(r);
    EXPECT_EQ(0, r.filesWritten) << renderResult(r);

    // And the KEYS are absent, not zero: a caller reading a stored envelope has
    // only their presence to tell "nothing needed repairing" from "never asked".
    const QJsonObject env = RemoteControl::docLintBuildResponse(r, 500, /*fix=*/false);
    EXPECT_FALSE(env.contains(QStringLiteral("fixed"))) << render(env);
    EXPECT_FALSE(env.contains(QStringLiteral("files_written"))) << render(env);
    EXPECT_FALSE(env.contains(QStringLiteral("dry_run"))) << render(env);
}

// ---------------------------------------------------------------------------
// INV-5 — only what carries the flag is repaired, and the two counts differ.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv5OnlyAutoFixableAndTheTwoCounts) {
    Tree t;
    t.write(QStringLiteral("docs/rich.md"), kRich);

    const DocLint::Result r =
        DocLint::run({QStringLiteral("docs/rich.md")}, optsFor(t, /*fix=*/true));

    // kRich carries a dead anchor (`#ghost`) as well as the TOC gap. Only the
    // flagged one may be touched.
    bool sawUnfixable = false;
    for (const DocFinding::Finding &f : r.findings)
        if (!f.autoFixable) sawUnfixable = true;
    ASSERT_TRUE(sawUnfixable) << "fixture lost its non-fixable finding: " << renderResult(r);

    for (const DocFinding::Finding &f : r.fixed)
        EXPECT_TRUE(f.autoFixable) << "repaired a finding that is not auto-fixable: "
                                   << renderResult(r);
    EXPECT_EQ(QStringLiteral("toc_gap"), r.fixed.isEmpty() ? QString() : r.fixed.first().kind)
        << renderResult(r);

    // The dead entry survives: doc_integrity never reports entry->heading, so
    // deleting it would be the fixer acting on a judgement no checker made.
    EXPECT_TRUE(t.read(QStringLiteral("docs/rich.md"))
                    .contains(QStringLiteral("- [Ghost](#ghost)")))
        << "the fixer removed an entry nothing reported";

    // fixed[] is per FINDING, files_written per FILE.
    Tree t2;
    t2.write(QStringLiteral("docs/three.md"), kThreeGaps);
    const DocLint::Result r2 =
        DocLint::run({QStringLiteral("docs/three.md")}, optsFor(t2, /*fix=*/true));
    EXPECT_EQ(3, r2.fixed.size()) << renderResult(r2);
    EXPECT_EQ(1, r2.filesWritten) << renderResult(r2);
}

// The gate is the FLAG, never the kind. While toc_gap is the only fixable kind
// the two are behaviourally identical, so no fixture can separate them — this
// is the arm that can.
TEST(DocLintFix, Inv5GateIsTheFlagNotTheKind) {
    const QString src = slurp(QStringLiteral(ANTS_DOCLINT_CPP_PATH));
    ASSERT_FALSE(src.isEmpty()) << "could not read the engine source";
    EXPECT_TRUE(src.contains(QStringLiteral("if (!f.autoFixable) continue;")))
        << "the fix path no longer gates on the autoFixable FLAG; a gate on "
           "kind == \"toc_gap\" passes every fixture in this file and breaks the "
           "moment a producer marks another kind fixable";
}

// ---------------------------------------------------------------------------
// INV-18 — the repair is a PATCH, never a regeneration.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv18TocRepairIsAPatch) {
    Tree t;
    t.write(QStringLiteral("docs/rich.md"), kRich);

    const DocLint::Result r =
        DocLint::run({QStringLiteral("docs/rich.md")}, optsFor(t, /*fix=*/true));
    ASSERT_EQ(1, r.filesWritten) << renderResult(r);

    const QString after = t.read(QStringLiteral("docs/rich.md"));

    // The gap is closed.
    EXPECT_TRUE(after.contains(QStringLiteral("- [Beta](#beta)"))) << after.toStdString();

    // And every row a regeneration from the H2 set would have eaten survives,
    // byte-identical. These three are the whole point of the fixture.
    EXPECT_TRUE(after.contains(QStringLiteral("  - [Sub thing](#sub-thing)")))
        << "an H3 child entry was destroyed";
    EXPECT_TRUE(after.contains(QStringLiteral("- see also the appendix")))
        << "a free-text row was destroyed";
    EXPECT_TRUE(after.contains(QStringLiteral("- [Ghost](#ghost)")))
        << "an entry matching no heading was destroyed";

    // Inserted in document order, after the section that precedes it rather
    // than appended to the end of the region.
    EXPECT_LT(after.indexOf(QStringLiteral("- [Beta](#beta)")),
              after.indexOf(QStringLiteral("- [Gamma](#gamma)")))
        << after.toStdString();
}

// A region with no top-level entry to copy is not repaired at all.
TEST(DocLintFix, Inv18NoTemplateRefusesRatherThanGuessing) {
    Tree t;
    t.write(QStringLiteral("docs/nt.md"), kNoTemplate);

    const DocLint::Result r =
        DocLint::run({QStringLiteral("docs/nt.md")}, optsFor(t, /*fix=*/true));

    EXPECT_EQ(kNoTemplate, t.read(QStringLiteral("docs/nt.md")))
        << "guessed a list convention the author did not choose";
    EXPECT_TRUE(r.fixed.isEmpty()) << renderResult(r);
    EXPECT_EQ(0, r.filesWritten) << renderResult(r);
    EXPECT_TRUE(reasonsOf(r).contains(QStringLiteral("no_template"))) << renderResult(r);
}

// ---------------------------------------------------------------------------
// INV-12 — dry_run computes the same envelope down the same path, and writes
// nothing.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv12DryRunMatchesRealEnvelope) {
    Tree dry;
    dry.write(QStringLiteral("docs/a.md"), kSimple);
    const DocLint::Result rDry =
        DocLint::run({QStringLiteral("docs/a.md")}, optsFor(dry, true, /*dryRun=*/true));

    EXPECT_EQ(kSimple, dry.read(QStringLiteral("docs/a.md")))
        << "dry_run wrote to disk";

    Tree wet;
    wet.write(QStringLiteral("docs/a.md"), kSimple);
    const DocLint::Result rWet =
        DocLint::run({QStringLiteral("docs/a.md")}, optsFor(wet, true, /*dryRun=*/false));
    ASSERT_NE(kSimple, wet.read(QStringLiteral("docs/a.md")))
        << "the real run wrote nothing, so this row would compare two previews";

    QJsonObject envDry = RemoteControl::docLintBuildResponse(rDry, 500, true, true);
    const QJsonObject envWet = RemoteControl::docLintBuildResponse(rWet, 500, true, false);

    // The echo is the ONLY permitted difference — it is what lets a caller
    // reading a stored envelope tell a preview from a run that really wrote.
    EXPECT_TRUE(envDry.contains(QStringLiteral("dry_run"))) << render(envDry);
    EXPECT_FALSE(envWet.contains(QStringLiteral("dry_run"))) << render(envWet);
    envDry.remove(QStringLiteral("dry_run"));
    EXPECT_EQ(render(envWet), render(envDry))
        << "the preview diverged from the run it previews";
}

// ---------------------------------------------------------------------------
// INV-15 — max_findings pages the findings, never the repairs.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv15CapDoesNotBoundRepairs) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kSimple);
    t.write(QStringLiteral("docs/b.md"), kSimple);

    const DocLint::Result r = DocLint::run(
        {QStringLiteral("docs/a.md"), QStringLiteral("docs/b.md")}, optsFor(t, true));

    ASSERT_EQ(2, r.filesWritten) << renderResult(r);
    ASSERT_EQ(2, r.fixed.size()) << renderResult(r);

    // A cap of one page must not reach the repairs — letting it would make the
    // number of documents WRITTEN depend on a display argument.
    const QJsonObject env = RemoteControl::docLintBuildResponse(r, /*maxFindings=*/1, true);
    EXPECT_EQ(1, env.value(QStringLiteral("findings")).toArray().size()) << render(env);
    EXPECT_EQ(2, env.value(QStringLiteral("files_written")).toInt()) << render(env);
    EXPECT_EQ(2, env.value(QStringLiteral("fixed")).toArray().size()) << render(env);

    // § 2.3's element shape: EXACTLY four fields. `message` and `auto_fixable`
    // are absent as well as `emission_index` — this array reports what was
    // repaired, not what the finding said, and it is not a join key.
    const QJsonObject el = env.value(QStringLiteral("fixed")).toArray().at(0).toObject();
    EXPECT_EQ(4, el.size()) << render(el);
    for (const QString &k : {QStringLiteral("verb"), QStringLiteral("kind"),
                             QStringLiteral("file"), QStringLiteral("line")})
        EXPECT_TRUE(el.contains(k)) << k.toStdString() << " missing: " << render(el);

    // Both documents really were patched on disk.
    EXPECT_TRUE(t.read(QStringLiteral("docs/a.md")).contains(QStringLiteral("(#beta)")));
    EXPECT_TRUE(t.read(QStringLiteral("docs/b.md")).contains(QStringLiteral("(#beta)")));
}

// ---------------------------------------------------------------------------
// INV-14 — a write that fails is contained, and reports nothing as repaired.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv14WriteFailureIsContained) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kSimple);

    // QSaveFile writes a temporary beside the target and renames, so it is the
    // DIRECTORY that has to refuse. Restored below whatever the assertions do —
    // QTemporaryDir cannot clean up a directory it may not write.
    const QString docsDir = t.abs(QStringLiteral("docs"));
    ASSERT_TRUE(QFile::setPermissions(
        docsDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    const DocLint::Result r =
        DocLint::run({QStringLiteral("docs/a.md")}, optsFor(t, /*fix=*/true));

    QFile::setPermissions(docsDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner);

    EXPECT_TRUE(reasonsOf(r).contains(QStringLiteral("write_failed"))) << renderResult(r);
    // The absence is the assertion: appending on INTENT rather than on success
    // reports a repair that never happened and looks correct doing it.
    EXPECT_TRUE(r.fixed.isEmpty()) << renderResult(r);
    EXPECT_EQ(0, r.filesWritten) << renderResult(r);
    EXPECT_EQ(kSimple, t.read(QStringLiteral("docs/a.md"))) << "the original was damaged";
}

// ---------------------------------------------------------------------------
// INV-21 — the write re-reads and re-derives; divergence is refused per
// document.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv21StaleDocumentIsRefusedWhole) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kSimple);

    // The seam § 6 requires: the walk has finished and the fix path has opened
    // nothing, which is exactly the window a user's save falls into.
    const QString closed = QString(kSimple).replace(
        QStringLiteral("- [Gamma](#gamma)"),
        QStringLiteral("- [Beta](#beta)\n- [Gamma](#gamma)"));
    DocLint::Options o = optsFor(t, /*fix=*/true);
    o.afterWalkHook = [&] { t.write(QStringLiteral("docs/a.md"), closed); };

    const DocLint::Result r = DocLint::run({QStringLiteral("docs/a.md")}, o);

    EXPECT_TRUE(reasonsOf(r).contains(QStringLiteral("stale"))) << renderResult(r);
    EXPECT_TRUE(r.fixed.isEmpty()) << renderResult(r);
    EXPECT_EQ(0, r.filesWritten) << renderResult(r);
    // The user's save survives untouched — this is the whole point.
    EXPECT_EQ(closed, t.read(QStringLiteral("docs/a.md")))
        << "the fixer wrote over a document that had changed under it";
}

// The row that actually PROVES the staleness check, rather than observing its
// outcome. The missing-section arm above passes even with the check deleted,
// because the patcher's own lookup finds no uncovered heading and refuses on
// its own — two guards, one outcome, and a fixture that cannot tell them apart.
// A DUPLICATE gap can: that branch deletes BY LINE NUMBER, so without the check
// it removes whatever now occupies the reported line.
TEST(DocLintFix, Inv21StaleDuplicateDoesNotDeleteAnInnocentLine) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kDuplicate);

    DocLint::Options o = optsFor(t, /*fix=*/true);
    o.afterWalkHook = [&] { t.write(QStringLiteral("docs/a.md"), kDuplicateFixed); };

    const DocLint::Result r = DocLint::run({QStringLiteral("docs/a.md")}, o);

    EXPECT_TRUE(reasonsOf(r).contains(QStringLiteral("stale"))) << renderResult(r);
    EXPECT_EQ(0, r.filesWritten) << renderResult(r);
    // The reported line now holds `- [Beta](#beta)`. Deleting it would be
    // silent data loss that leaves a document doc_integrity then calls broken.
    EXPECT_EQ(kDuplicateFixed, t.read(QStringLiteral("docs/a.md")))
        << "a line the walk never reported was deleted";
}

// The duplicate branch's happy path, so the row above cannot pass merely
// because duplicates are never repaired at all.
TEST(DocLintFix, Inv18DuplicateEntryIsDeletedKeepingTheFirst) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kDuplicate);

    const DocLint::Result r =
        DocLint::run({QStringLiteral("docs/a.md")}, optsFor(t, /*fix=*/true));

    ASSERT_EQ(1, r.filesWritten) << renderResult(r);
    EXPECT_EQ(kDuplicateFixed, t.read(QStringLiteral("docs/a.md")))
        << "the duplicate repair did not keep the first occurrence";
}

TEST(DocLintFix, Inv21ShiftedLinesStillRepair) {
    Tree t;
    t.write(QStringLiteral("docs/a.md"), kSimple);

    // Same gap, every line below the insertion moved. A fixer that patches the
    // text the WALK read writes back a document without these lines.
    const QString shifted = QStringLiteral("<!-- added -->\n\n") + kSimple;
    DocLint::Options o = optsFor(t, /*fix=*/true);
    o.afterWalkHook = [&] { t.write(QStringLiteral("docs/a.md"), shifted); };

    const DocLint::Result r = DocLint::run({QStringLiteral("docs/a.md")}, o);

    EXPECT_EQ(1, r.filesWritten) << renderResult(r);
    const QString after = t.read(QStringLiteral("docs/a.md"));
    EXPECT_TRUE(after.startsWith(QStringLiteral("<!-- added -->")))
        << "the repair was derived from the walk's stale text, not from the file";
    EXPECT_TRUE(after.contains(QStringLiteral("- [Beta](#beta)"))) << after.toStdString();
}

// ---------------------------------------------------------------------------
// INV-16 — dry_run without fix refuses. cmdDocLint needs a live MainWindow, so
// the guard is scraped where it lives.
// ---------------------------------------------------------------------------
TEST(DocLintFix, Inv16DryRunRequiresFix) {
    QString src;
    for (const QString &p : QString::fromUtf8(ANTS_RC_SOURCES).split(QLatin1Char(';'))) {
        const QString body = slurp(p);
        if (body.contains(QStringLiteral("RemoteControl::cmdDocLint"))) { src = body; break; }
    }
    ASSERT_FALSE(src.isEmpty()) << "could not find the translation unit holding cmdDocLint";

    EXPECT_TRUE(src.contains(QStringLiteral("if (dryRun && !wantFix)")))
        << "dry_run without fix is no longer refused; the fix keys are absent "
           "under fix:false, so a silently-accepted preview returns exactly the "
           "envelope of a plain read";
    EXPECT_TRUE(src.contains(QStringLiteral("dry_run requires fix:true")))
        << "the refusal no longer says which pair was wrong";
}
