// Feature-conformance test for the apply_edits MCP tool (ANTS-2022).
// Behavioural invariants exercise the pure ApplyEdits::applyToContent
// helper; wiring invariants source-scrape the registration sites.
// See spec.md + docs/specs/ANTS-2022.md.

#include "../../_support/expect.h"
#include "applyedits.h"
#include "remotecontrol.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// W1 — wiring across the registration sites.
TEST(McpApplyEdits, WiringContract) {
    expect_reset();
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    expect(has(rcHdr, "cmdApplyEdits(const QJsonObject &req)"), "W1 decl",
           "remotecontrol.h missing cmdApplyEdits declaration");
    expect(has(rcCpp, "cmdApplyEdits") && has(rcCpp, "ApplyEdits::applyToContent"),
           "W1 impl", "remotecontrol.cpp cmdApplyEdits must call ApplyEdits::applyToContent");
    expect(has(rcCpp, "validatePath") && has(rcCpp, "apply_edits"),
           "W1 pathvalidation", "cmdApplyEdits must validate each path arg");
    expect(has(rcCpp, "QSaveFile") && has(rcCpp, "fsyncParentDir"),
           "W1 atomic", "cmdApplyEdits must write atomically (QSaveFile + fsyncParentDir)");
    expect(has(ciCpp, "\"apply_edits\""), "W1 schema",
           "claudeintegration.cpp missing apply_edits registration");
    expect(has(ciCpp, "callerCwdContractFor") &&
           has(ciCpp, "C::Required") && has(ciCpp, "apply_edits"),
           "W1 Required", "apply_edits must be callerCwdContractFor → Required");
    expect(has(ciCpp, "\"edits\""), "W1 prop edits",
           "apply_edits schema missing the edits property");
    expect(has(mwCpp, "registerToolProvider(\"apply_edits\"") &&
           has(mwCpp, "cmdApplyEdits"), "W1 dispatch",
           "mainwindow.cpp must register apply_edits → cmdApplyEdits");
    EXPECT_EQ(0, expect_failures());
}

// B1 — unique / absent / duplicate `old` (INV-1).
TEST(McpApplyEdits, UniqueAbsentDuplicate) {
    // Unique → applied.
    auto u = ApplyEdits::applyToContent("alpha beta gamma", "beta", "BETA", false);
    EXPECT_TRUE(u.applied);
    EXPECT_EQ(u.replacements, 1);
    EXPECT_EQ(u.newContents, "alpha BETA gamma");

    // Absent → not_found.
    auto a = ApplyEdits::applyToContent("alpha beta", "zeta", "Z", false);
    EXPECT_FALSE(a.applied);
    EXPECT_EQ(a.skipReason, "not_found");

    // Duplicate without replace_all → ambiguous.
    auto d = ApplyEdits::applyToContent("x x x", "x", "y", false);
    EXPECT_FALSE(d.applied);
    EXPECT_EQ(d.skipReason, "ambiguous");
}

// ANTS-4418 — a `not_found` caused by whitespace-only drift names the line.
// The reported case verbatim: `old` was copied from a workspace_search result
// and differed from the file only in the run-length of interior spaces, because
// the file aligns a trailing-comment column. `not_found` alone is equally
// consistent with "the text is gone", "wrong file" and "you are one space out",
// and this is the verb where the third is most likely — while its siblings
// (read_region section-mode, roadmap_log bullet locators) already return
// `candidates` on a miss.
TEST(McpApplyEdits, Ants4418WhitespaceNearMissNamesTheLine) {
    const QString file =
        QStringLiteral("#!/bin/sh\n"
                       "python3 tools/verify_watch.py     # INV-32: check\n"
                       "echo done\n");
    // Three interior spaces where the file has five.
    const auto r = ApplyEdits::applyToContent(
        file,
        QStringLiteral("python3 tools/verify_watch.py   # INV-32: check"),
        QStringLiteral("python3 tools/verify_watch.py   # INV-33: check"),
        false);
    EXPECT_FALSE(r.applied);
    EXPECT_EQ(r.skipReason, "not_found");
    EXPECT_EQ(r.nearMissLine, 2)
        << "the whitespace-only near miss must be located, 1-based, so the "
           "start_line/end_line form is usable as the immediate retry";
    EXPECT_EQ(r.nearMissKind, "whitespace");
    EXPECT_EQ(r.nearMissText,
              QStringLiteral("python3 tools/verify_watch.py     # INV-32: check"))
        << "the reported text must be the FILE's bytes, so a caller can retry "
           "with it verbatim rather than guessing the spacing again";
}

// ANTS-4418 — the near miss is reported only when it is UNIQUE, and only for a
// whitespace difference. Two candidates cannot tell the caller which to retry,
// so naming one arbitrarily is worse than naming none; and a genuinely absent
// string must not acquire a spurious "did you mean" line.
TEST(McpApplyEdits, Ants4418NearMissOnlyWhenUniqueAndWhitespace) {
    // Two lines normalise to the same thing → no near miss.
    const auto ambiguous = ApplyEdits::applyToContent(
        QStringLiteral("a  b\na    b\n"), QStringLiteral("a b"),
        QStringLiteral("c"), false);
    EXPECT_EQ(ambiguous.skipReason, "not_found");
    EXPECT_EQ(ambiguous.nearMissLine, -1)
        << "two whitespace-equal candidates must report none";

    // Genuinely absent → no near miss.
    const auto absent = ApplyEdits::applyToContent(
        QStringLiteral("alpha\nbeta\n"), QStringLiteral("gamma"),
        QStringLiteral("g"), false);
    EXPECT_EQ(absent.skipReason, "not_found");
    EXPECT_EQ(absent.nearMissLine, -1)
        << "an absent string must not acquire a spurious near miss";

    // A difference that is NOT whitespace must not be reported as one.
    const auto typo = ApplyEdits::applyToContent(
        QStringLiteral("value = 42\n"), QStringLiteral("value = 43"),
        QStringLiteral("value = 44"), false);
    EXPECT_EQ(typo.nearMissLine, -1)
        << "a value difference is not a whitespace near miss; claiming it is "
           "would send the caller to re-copy text that is genuinely different";

    // A multi-line `old` reports nothing rather than a confident wrong line —
    // the documented scope limit.
    const auto multi = ApplyEdits::applyToContent(
        QStringLiteral("one  two\nthree\n"),
        QStringLiteral("one two\nthree"), QStringLiteral("x"), false);
    EXPECT_EQ(multi.nearMissLine, -1)
        << "multi-line near-miss alignment is out of scope and must report "
           "nothing rather than guess";
}

// B2 — replace_all (INV-2).
TEST(McpApplyEdits, ReplaceAll) {
    auto r = ApplyEdits::applyToContent("a.a.a", "a", "b", true);
    EXPECT_TRUE(r.applied);
    EXPECT_EQ(r.replacements, 3);
    EXPECT_EQ(r.newContents, "b.b.b");

    // replace_all with 0 occurrences still skips not_found (no silent no-op).
    auto z = ApplyEdits::applyToContent("nothing", "q", "Q", true);
    EXPECT_FALSE(z.applied);
    EXPECT_EQ(z.skipReason, "not_found");
}

// B3 — trailing newline preserved by whole-content substring replace (INV-8).
TEST(McpApplyEdits, TrailingNewline) {
    // File ending in a newline keeps exactly one.
    auto withNl = ApplyEdits::applyToContent("foo\nbar\n", "bar", "baz", false);
    ASSERT_TRUE(withNl.applied);
    EXPECT_EQ(withNl.newContents, "foo\nbaz\n");
    EXPECT_TRUE(withNl.newContents.endsWith('\n'));

    // File with no final newline keeps none.
    auto noNl = ApplyEdits::applyToContent("foo\nbar", "bar", "baz", false);
    ASSERT_TRUE(noNl.applied);
    EXPECT_EQ(noNl.newContents, "foo\nbaz");
    EXPECT_FALSE(noNl.newContents.endsWith('\n'));
}

// ---- ANTS-3711 — line-range selector ---------------------------------------

namespace {

const char *kDoc =
    "alpha\n"
    "beta\n"
    "gamma\n"
    "delta\n"
    "epsilon\n";

QString seedDoc(const QTemporaryDir &dir, const char *body = kDoc) {
    const QString p = dir.path() + QStringLiteral("/doc.txt");
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body);
    return p;
}

QString slurp(const QString &p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// One range edit over doc.txt, with whatever fields the caller supplies.
QJsonObject runEdit(const QString &root, const QJsonObject &edit) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    QJsonArray edits; edits.append(edit);
    req[QStringLiteral("edits")] = edits;
    return rc.cmdApplyEdits(req).object();
}

QJsonObject rangeEdit(int from, int to, const QString &first,
                      const QString &last, const QString &repl) {
    QJsonObject e;
    e[QStringLiteral("path")]              = QStringLiteral("doc.txt");
    e[QStringLiteral("start_line")]        = from;
    e[QStringLiteral("end_line")]          = to;
    e[QStringLiteral("expect_first_line")] = first;
    e[QStringLiteral("expect_last_line")]  = last;
    e[QStringLiteral("new")]               = repl;
    return e;
}

}  // namespace

// INV-9 — a guarded range replaces exactly those lines, and a trailing newline
// survives (the whole-content property INV-8 pins for the `old` path).
TEST(McpApplyEdits, Ants3711RangeReplacesAndKeepsTrailingNewline) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedDoc(dir); ASSERT_FALSE(p.isEmpty());

    const QJsonObject env = runEdit(
        dir.path(), rangeEdit(2, 4, QStringLiteral("beta"),
                              QStringLiteral("delta"),
                              QStringLiteral("BETA\nDELTA")));
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("edits_applied").toInt(), 1);
    EXPECT_EQ(env.value("edits_skipped").toInt(), 0);
    EXPECT_EQ(slurp(p), QStringLiteral("alpha\nBETA\nDELTA\nepsilon\n"));
}

// INV-10 — an empty `new` DELETES the range. A blank line left behind would be
// a silently wrong answer to "delete these 76 lines", the motivating case.
TEST(McpApplyEdits, Ants3711EmptyNewDeletesTheRange) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedDoc(dir); ASSERT_FALSE(p.isEmpty());

    const QJsonObject env = runEdit(
        dir.path(), rangeEdit(2, 4, QStringLiteral("beta"),
                              QStringLiteral("delta"), QString()));
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("edits_applied").toInt(), 1);
    EXPECT_EQ(slurp(p), QStringLiteral("alpha\nepsilon\n"));
}

// INV-11 — the staleness guard. A number that no longer names the line the
// caller thinks it does must refuse LOUDLY and leave the file untouched; this
// is the entire difference between this and the Bash line splice it replaces.
TEST(McpApplyEdits, Ants3711StaleRangeRefusesAndLeavesFileIntact) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedDoc(dir); ASSERT_FALSE(p.isEmpty());
    const QString before = slurp(p);

    // Right shape, wrong coordinates — the range has drifted by one.
    const QJsonObject env = runEdit(
        dir.path(), rangeEdit(3, 5, QStringLiteral("beta"),
                              QStringLiteral("delta"),
                              QStringLiteral("X")));
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("edits_applied").toInt(), 0);
    ASSERT_EQ(env.value("skipped").toArray().size(), 1);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject()
                  .value("reason").toString(),
              QStringLiteral("range_mismatch"));
    EXPECT_EQ(slurp(p), before) << "a refused range must not write";

    // Past EOF is a distinct reason — out of bounds, not a mismatch.
    const QJsonObject oob = runEdit(
        dir.path(), rangeEdit(5, 99, QStringLiteral("epsilon"),
                              QStringLiteral("nope"), QStringLiteral("X")));
    ASSERT_EQ(oob.value("skipped").toArray().size(), 1);
    EXPECT_EQ(oob.value("skipped").toArray().at(0).toObject()
                  .value("reason").toString(),
              QStringLiteral("range_out_of_bounds"));
    EXPECT_EQ(slurp(p), before);
}

// INV-12 — the selectors are mutually exclusive, and a range without its
// guards is refused up front rather than applied unguarded. `bad_args` (a
// whole-call refusal), not a per-edit skip: the request is malformed, not the
// file surprising.
TEST(McpApplyEdits, Ants3711SelectorArgumentRules) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_FALSE(seedDoc(dir).isEmpty());

    // Both selectors.
    QJsonObject both = rangeEdit(1, 2, QStringLiteral("alpha"),
                                 QStringLiteral("beta"), QStringLiteral("X"));
    both[QStringLiteral("old")] = QStringLiteral("alpha");
    const QJsonObject e1 = runEdit(dir.path(), both);
    EXPECT_FALSE(e1.value("ok").toBool());
    EXPECT_EQ(e1.value("code").toString(), QStringLiteral("bad_args"));

    // Neither selector.
    QJsonObject none;
    none[QStringLiteral("path")] = QStringLiteral("doc.txt");
    none[QStringLiteral("new")]  = QStringLiteral("X");
    const QJsonObject e2 = runEdit(dir.path(), none);
    EXPECT_FALSE(e2.value("ok").toBool());
    EXPECT_EQ(e2.value("code").toString(), QStringLiteral("bad_args"));

    // A range missing its guards — the shape that would corrupt on stale
    // coordinates, so it never reaches the file.
    QJsonObject bare;
    bare[QStringLiteral("path")]       = QStringLiteral("doc.txt");
    bare[QStringLiteral("start_line")] = 2;
    bare[QStringLiteral("end_line")]   = 3;
    bare[QStringLiteral("new")]        = QStringLiteral("X");
    const QJsonObject e3 = runEdit(dir.path(), bare);
    EXPECT_FALSE(e3.value("ok").toBool());
    EXPECT_EQ(e3.value("code").toString(), QStringLiteral("bad_args"));
    EXPECT_TRUE(e3.value("error").toString().contains("expect_first_line"));

    // Half a range.
    QJsonObject half;
    half[QStringLiteral("path")]       = QStringLiteral("doc.txt");
    half[QStringLiteral("start_line")] = 2;
    half[QStringLiteral("new")]        = QStringLiteral("X");
    const QJsonObject e4 = runEdit(dir.path(), half);
    EXPECT_FALSE(e4.value("ok").toBool());
    EXPECT_EQ(e4.value("code").toString(), QStringLiteral("bad_args"));
}

// INV-13 — range and `old` edits compose in array order against the SAME
// working content, so an earlier edit that changes the line count shifts a
// later range onto different text. The guard is what makes that visible: this
// is a range_mismatch skip, not a silent write to the wrong lines.
TEST(McpApplyEdits, Ants3711EarlierEditShiftsLaterRangeAndTheGuardCatchesIt) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedDoc(dir); ASSERT_FALSE(p.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = dir.path();
    QJsonArray edits;
    // Edit 0 removes a line, so every line below moves up by one.
    QJsonObject drop;
    drop[QStringLiteral("path")] = QStringLiteral("doc.txt");
    drop[QStringLiteral("old")]  = QStringLiteral("beta\n");
    drop[QStringLiteral("new")]  = QString();
    edits.append(drop);
    // Edit 1 names coordinates measured against the ORIGINAL file.
    edits.append(rangeEdit(4, 4, QStringLiteral("delta"),
                           QStringLiteral("delta"), QStringLiteral("DELTA")));
    req[QStringLiteral("edits")] = edits;

    const QJsonObject env = rc.cmdApplyEdits(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("edits_applied").toInt(), 1);   // the deletion only
    ASSERT_EQ(env.value("skipped").toArray().size(), 1);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject()
                  .value("reason").toString(),
              QStringLiteral("range_mismatch"));
    // Line 4 after the deletion is "epsilon", not "delta" — and it survives.
    EXPECT_EQ(slurp(p), QStringLiteral("alpha\ngamma\ndelta\nepsilon\n"));
}
