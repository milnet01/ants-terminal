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
}
