// ANTS-4398 — mutation_probe. Engine conformance + wiring.
//
// The verb exists because several projects' CLAUDE.md files mandate mutating
// an invariant before believing it is held, and nothing served that loop —
// so every session hand-rolled ~40 lines of bash. One reporting project wrote
// it three times in a single session; this project six times in one evening.

#include "mutationprobe.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QString>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {
MutationProbe::Mutation mk(const char *l, const char *o, const char *n) {
    MutationProbe::Mutation m;
    m.label   = QString::fromUtf8(l);
    m.oldText = QString::fromUtf8(o);
    m.newText = QString::fromUtf8(n);
    return m;
}
}  // namespace

// INV-1 — the three INERT shapes, which is what the verb is for.
//
// From the outside an inert mutation and a surviving mutant are
// indistinguishable, and the wrong reading is "my test is weak" when the
// truth is "my patch never applied". One session hit three in a row: a
// comment-only edit, a `[... for x in []]` no-op, and a half-applied two-part
// sed. Each initially read as "the suite holds this" and each was false.
TEST(MutationProbe, Inv1InertShapes) {
    const QString src = QStringLiteral("int limit = 3;\nreturn limit;\n");

    // (a) `old` is absent — the commonest, and the dangerous one: the test run
    //     that follows would be against UNMUTATED code, so it passes.
    const auto absent = MutationProbe::applyOne(src, mk("a", "nosuchtext", "x"));
    EXPECT_FALSE(absent.ok);
    EXPECT_EQ(absent.inert, MutationProbe::Inert::OldTextAbsent);
    EXPECT_EQ(absent.occurrences, 0);

    // (b) the replacement is the original — bytes identical.
    const auto same = MutationProbe::applyOne(src, mk("b", "limit", "limit"));
    EXPECT_FALSE(same.ok);
    EXPECT_EQ(same.inert, MutationProbe::Inert::Unchanged);

    // (c) an empty `old` would match at every position and splice `new`
    //     between every character. Refused as its own reason rather than as a
    //     generic bad_args, so one malformed entry cannot lose the batch.
    const auto empty = MutationProbe::applyOne(src, mk("c", "", "x"));
    EXPECT_FALSE(empty.ok);
    EXPECT_EQ(empty.inert, MutationProbe::Inert::OldTextEmpty);

    // Control — a real mutation applies, and reports how many it hit so a
    // caller can see it caught more than expected.
    const auto real = MutationProbe::applyOne(src, mk("d", "limit", "cap"));
    ASSERT_TRUE(real.ok);
    EXPECT_EQ(real.inert, MutationProbe::Inert::No);
    EXPECT_EQ(real.occurrences, 2);
    EXPECT_EQ(real.patched, QStringLiteral("int cap = 3;\nreturn cap;\n"));
}

// INV-2 — counts are parsed from the three runners in use, and an
// unrecognised output leaves them at -1.
//
// -1 is NOT 0, and the difference is load-bearing: a run whose output could
// not be parsed has not told the caller that nothing passed, and reporting 0
// would be a confident wrong answer of exactly the kind this verb prevents.
TEST(MutationProbe, Inv2CountParsing) {
    const auto py = MutationProbe::parseCounts(
        QStringLiteral("=== 3 failed, 5 passed in 1.23s ==="));
    EXPECT_EQ(py.passed, 5);
    EXPECT_EQ(py.failed, 3);

    // pytest names only the non-zero buckets, so an absent half IS zero here.
    const auto pyClean = MutationProbe::parseCounts(
        QStringLiteral("=== 12 passed in 0.4s ==="));
    EXPECT_EQ(pyClean.passed, 12);
    EXPECT_EQ(pyClean.failed, 0);

    const auto ct = MutationProbe::parseCounts(
        QStringLiteral("97% tests passed, 2 tests failed out of 42"));
    EXPECT_EQ(ct.failed, 2);
    EXPECT_EQ(ct.passed, 40);

    const auto gt = MutationProbe::parseCounts(
        QStringLiteral("[  PASSED  ] 5 tests.\n[  FAILED  ] 2 tests"));
    EXPECT_EQ(gt.passed, 5);
    EXPECT_EQ(gt.failed, 2);

    const auto unknown = MutationProbe::parseCounts(
        QStringLiteral("Segmentation fault (core dumped)"));
    EXPECT_EQ(unknown.passed, -1)
        << "unparsed output must stay -1 — reporting 0 would claim nothing "
           "passed, which the output does not say";
    EXPECT_EQ(unknown.failed, -1);
}

// INV-3 — the guarantees a shell loop cannot make are actually wired.
TEST(MutationProbe, Inv3GuaranteesWired) {
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Registered, and on the WORKER delegate — each mutation is a full test
    // run, so a batch is seconds-to-minutes and must not block the GUI thread.
    EXPECT_NE(mw.find("registerToolProvider(\"mutation_probe\""),
              std::string::npos);
    EXPECT_NE(mw.find("rcDelegateWorker(&RemoteControl::cmdMutationProbe)"),
              std::string::npos)
        << "a batch of full test runs must not run on the GUI thread";
    EXPECT_NE(ci.find("\"mutation_probe\""), std::string::npos);

    // The restore is verified, not assumed — a leaked mutated file in a repo
    // the session then commits is the dangerous failure.
    EXPECT_NE(rc.find("restored_clean"), std::string::npos);
    // An inert mutation must skip the run entirely rather than run and
    // disclaim: a green result against unmutated code is the false reading.
    EXPECT_NE(rc.find("\"inert\""), std::string::npos);
    EXPECT_NE(rc.find("baseline_not_green"), std::string::npos)
        << "a red baseline makes every result meaningless — a mutant dying "
           "proves nothing when the suite was already failing";

    // test_command is argv, never a shell string. This verb writes to a
    // source file and spawns a process; a shell string would make it an
    // arbitrary-command surface reachable from a tool call.
    EXPECT_EQ(rc.find("QProcess::startCommand"), std::string::npos)
        << "no shell-string execution on this path";
    EXPECT_NE(rc.find("ARGV ARRAY"), std::string::npos)
        << "the refusal must say so, since a shell string is the natural "
           "thing a caller reaches for";

    // ANTS-4521 — the per-mutation `expect_occurrences` is declared in the
    // schema. The behavioural tests below prove the check; this proves a
    // caller can DISCOVER it, which for an opt-in guard is the whole value.
    EXPECT_NE(ci.find("expect_occurrences"), std::string::npos)
        << "an opt-in guard nobody is told about is not a guard";
}

// ANTS-4401 — `require_green_baseline` must be gated on EVIDENCE of green,
// not on the absence of red.
//
// Hit live on 2026-08-15 proving ANTS-3849's test red: the flag was set, the
// runner's output was in a format parseCounts does not recognise, the reply
// carried baseline_passed:-1 / baseline_failed:-1, and the batch ran anyway.
// The mutant was genuinely killed that time, which is luck rather than
// design — the gate whose whole job is to refuse an unproven baseline had
// stood down in precisely the case where the verb does not know.
TEST(MutationProbe, Ants4401BaselineGateNeedsEvidenceOfGreen) {
    using V = MutationProbe::BaselineVerdict;
    const auto judge = [](bool timedOut, int exit, int passed, int failed) {
        MutationProbe::Counts c;
        c.passed = passed;
        c.failed = failed;
        return MutationProbe::judgeBaseline(timedOut, exit, c);
    };

    // The two states this row was filed for. Both exit 0 and fail nothing,
    // and both used to satisfy the gate.
    EXPECT_EQ(judge(false, 0, -1, -1), V::Unreadable)
        << "an unparsable summary is not a green baseline — -1 was chosen "
           "over 0 precisely because the run did not say nothing passed";
    EXPECT_EQ(judge(false, 0, 0, 0), V::Empty)
        << "a run that executed nothing cannot be green; a gtest binary "
           "under a filter matching no test exits 0";

    // A HALF-parsed run is unreadable too. parseCounts fills the absent half
    // with 0 only when it recognised the format, so a lone -1 reaching here
    // means the format was not recognised at all.
    EXPECT_EQ(judge(false, 0, 5, -1), V::Unreadable);
    EXPECT_EQ(judge(false, 0, -1, 0), V::Unreadable);

    // The pre-existing red arms are unchanged — this widens the gate, it does
    // not move it.
    EXPECT_EQ(judge(true,  0, 10, 0), V::NotGreen) << "timed out";
    EXPECT_EQ(judge(false, 1, 10, 0), V::NotGreen) << "non-zero exit";
    // A non-zero exit wins over the counts, and a non-zero failure count wins
    // over a zero exit: the loudest evidence of red decides either way.
    EXPECT_EQ(judge(false, 1, -1, -1), V::NotGreen);
    EXPECT_EQ(judge(false, 0, 10, 2), V::NotGreen)
        << "a runner that reports failures and exits 0 is still red";

    // And the one state that IS green: it ran, something passed, nothing
    // failed. Without this arm every assertion above is satisfied by a
    // function that returns NotGreen unconditionally.
    EXPECT_EQ(judge(false, 0, 42, 0), V::Green);
    EXPECT_EQ(judge(false, 0, 1, 0), V::Green);
}

// The refusal must reach the wire, and must name which of the two silences it
// hit — the remedies differ (fix the runner's output vs fix the filter).
TEST(MutationProbe, Ants4401RefusalIsWiredAndDistinguishesTheTwoCauses) {
    const std::string rc = ants_test::slurpRemoteControl();
    EXPECT_NE(rc.find("baseline_unreadable"), std::string::npos)
        << "the new refusal code must be emitted by the verb";
    EXPECT_NE(rc.find("could not be parsed for pass/fail counts"),
              std::string::npos)
        << "the unparsable arm must say the counts could not be read";
    EXPECT_NE(rc.find("ran no tests"), std::string::npos)
        << "the empty arm must say nothing executed — pointing a caller at "
           "their runner's output format would be the wrong repair";
}

// ---------------------------------------------------------------------------
// ANTS-4521 — `expect_occurrences`: state how many sites the mutation meant.
//
// The verb refuses an inert mutation, and its reasoning is right: a mutation
// that changed nothing runs a test that passes against unmutated code. A
// mutation that changes MORE than the caller believes has the same shape and
// was not guarded.
//
// A LocalWebServerManager session meant to clear `high_contrast` on ONE
// palette; the literal occurs twice, both were cleared, and the result came
// back occurrences:2 outcome:killed. Nothing wrong followed because it died —
// and the dangerous direction is subtler than survival. With N sites mutated,
// the mutant can be killed by a test covering a site the caller never meant to
// touch while the site they DID mean to probe stays uncovered. The verdict
// reads `killed`, the label says what the caller intended, and the uncovered
// site is invisible. A false GREEN in a verb whose whole purpose is refusing
// false greens. `occurrences` was already reported, so the information existed
// — it just arrived as one integer among ten rather than as a check against
// intent. And the label is what gets quoted in a commit message as evidence.
//
// These drive the live verb rather than the engine, because the whole claim is
// about what the VERB does with the count applyOne already returns: refuse
// before the test runs, and therefore before a verdict exists to be misread.

#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

// Two sites for one literal — the reporter's shape exactly.
const char *kTwoSiteSource =
    "palette_a = {\n"
    "    'high_contrast': True,\n"
    "}\n"
    "palette_b = {\n"
    "    'high_contrast': True,\n"
    "}\n";

QString seedProject(const QTemporaryDir &tmp) {
    const QString p = QDir(tmp.path()).filePath(QStringLiteral("palettes.py"));
    QFile f(p);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(kTwoSiteSource);
    f.close();
    return p;
}

// `touch <sentinel>` as the test command: it exits 0 (so a run that DOES
// happen reads as `survived`) and leaves a file behind, which is what makes
// "no test was run" provable rather than inferred from a missing field.
QJsonObject probe(const QTemporaryDir &tmp, const QString &sentinel,
                  const QJsonObject &mutation) {
    const QString touch = QStandardPaths::findExecutable(QStringLiteral("touch"));
    EXPECT_FALSE(touch.isEmpty());
    QJsonArray argv; argv.append(touch); argv.append(sentinel);
    QJsonArray muts;  muts.append(mutation);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = tmp.path();
    req[QStringLiteral("path")]         = QStringLiteral("palettes.py");
    req[QStringLiteral("test_command")] = argv;
    req[QStringLiteral("mutations")]    = muts;
    return rc.cmdMutationProbe(req).object();
}

QJsonObject onlyResult(const QJsonObject &env) {
    const QJsonArray rs = env.value(QStringLiteral("results")).toArray();
    EXPECT_EQ(rs.size(), 1);
    return rs.isEmpty() ? QJsonObject() : rs.at(0).toObject();
}

QJsonObject mutation(int expect) {
    QJsonObject m;
    m[QStringLiteral("label")] = QStringLiteral("clear high_contrast on palette_a");
    m[QStringLiteral("old")]   = QStringLiteral("'high_contrast': True");
    m[QStringLiteral("new")]   = QStringLiteral("'high_contrast': False");
    if (expect > 0) m[QStringLiteral("expect_occurrences")] = expect;
    return m;
}

}  // namespace

// EO-1 — the reported call: one site intended, two found. The mutation is
// refused with its own outcome, both numbers are reported, and NO test ran.
TEST(MutationProbe, Ants4521OccurrenceMismatchRefusesBeforeAnyTestRuns) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = seedProject(tmp);
    const QString sentinel = QDir(tmp.path()).filePath(QStringLiteral("ran"));

    const QJsonObject env = probe(tmp, sentinel, mutation(1));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << "one bad mutation must not lose the batch — it is per-mutation, "
           "like inert: " << env.value(QStringLiteral("error")).toString().toStdString();

    const QJsonObject r = onlyResult(env);
    EXPECT_EQ(r.value(QStringLiteral("outcome")).toString().toStdString(),
              std::string("occurrence_mismatch"));
    EXPECT_FALSE(r.value(QStringLiteral("applied")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("occurrences")).toInt(), 2);
    EXPECT_EQ(r.value(QStringLiteral("expected_occurrences")).toInt(), 1);
    EXPECT_FALSE(r.contains(QStringLiteral("exit_code")))
        << "no verdict may exist for a mutation that was refused";

    EXPECT_FALSE(QFileInfo::exists(sentinel))
        << "the refusal must land BEFORE the test runs — that is the point of "
           "refusing rather than warning";

    // The file is untouched, so the batch is safe to re-issue.
    QFile f(src);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray(kTwoSiteSource));
    EXPECT_TRUE(env.value(QStringLiteral("restored_clean")).toBool());
}

// EO-2 — an expectation that MATCHES changes nothing: the mutation applies and
// the test runs exactly as it would without the field.
TEST(MutationProbe, Ants4521MatchingExpectationRunsNormally) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seedProject(tmp);
    const QString sentinel = QDir(tmp.path()).filePath(QStringLiteral("ran"));

    const QJsonObject r = onlyResult(probe(tmp, sentinel, mutation(2)));
    EXPECT_TRUE(r.value(QStringLiteral("applied")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("occurrences")).toInt(), 2);
    EXPECT_EQ(r.value(QStringLiteral("outcome")).toString().toStdString(),
              std::string("survived"));   // `touch` exits 0
    EXPECT_TRUE(QFileInfo::exists(sentinel));
}

// EO-3 — ABSENT is byte-identical to today. A multi-site mutation with no
// stated expectation is legitimate ("the constant 3" usually means all of
// them), which is why the field must NOT default to 1.
TEST(MutationProbe, Ants4521AbsentExpectationIsUnchangedBehaviour) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seedProject(tmp);
    const QString sentinel = QDir(tmp.path()).filePath(QStringLiteral("ran"));

    const QJsonObject r = onlyResult(probe(tmp, sentinel, mutation(0)));
    EXPECT_TRUE(r.value(QStringLiteral("applied")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("occurrences")).toInt(), 2);
    EXPECT_FALSE(r.contains(QStringLiteral("expected_occurrences")));
    EXPECT_EQ(r.value(QStringLiteral("outcome")).toString().toStdString(),
              std::string("survived"));
    EXPECT_TRUE(QFileInfo::exists(sentinel));
}
