// ANTS-4398 — `mutation_probe` engine.
//
// Pure (Qt6::Core only) half of the mutate-and-watch-it-go-red loop: apply a
// textual mutation, decide whether it actually CHANGED anything, and read a
// pass/fail count out of a test runner's output. No process spawning and no
// writes — `RemoteControl::cmdMutationProbe` owns those, so this file stays
// unit-testable without a project harness.
//
// Why the verb exists. Several projects' CLAUDE.md files mandate mutating an
// invariant before believing it is held, and nothing served that loop:
// `focused_test` is ctest-only and does not mutate, `invariant_check` reads
// specs and runs nothing. So every session hand-rolls the same ~40 lines of
// bash — one reporting project wrote it from scratch three times in a single
// session, and this project six times in one evening.
//
// **`inert` is the field that matters.** A mutation whose `old` text is
// absent, or whose replacement leaves the file byte-identical, must be
// reported as inert rather than as a surviving mutant: from the outside those
// two are indistinguishable, and the wrong reading is "my test is weak" when
// the truth is "my patch never applied". That misreading is not hypothetical
// — one session hit three inert mutations (a comment-only edit, a
// `[... for x in []]` no-op, and a half-applied two-part sed), each of which
// initially read as "the suite holds this" and each of which was false.
//
// What the loop buys when it works: that same session found 7 tests that were
// green and measured nothing across 58 mutants — a launcher that ignored
// SIGTERM, two fixtures already in sorted order for a sort-order test, a
// session-global object count, a one-row fixture against a per-row closure
// bug, and no fixture reaching two whole enum states. Reading found none of
// them.

#pragma once

#include <QString>
#include <QStringList>

namespace MutationProbe {

struct Mutation {
    QString label;     // caller's name for this mutant, echoed back
    QString oldText;   // exact substring to replace
    QString newText;   // replacement (may be empty — a deletion is a mutation)
};

// Why a mutation did not change the file. Empty when it did.
//
// These are NOT failures of the caller to be scolded for — they are the whole
// point of the field. A run that reports `inert` has told the caller
// something true and cheap; the alternative is a green test run that means
// nothing.
enum class Inert {
    No,             // the file changed
    OldTextAbsent,  // `old` does not occur — the commonest, and the one that
                    //   most looks like a surviving mutant
    Unchanged,      // `old` occurs but new == old, so the bytes are identical
    OldTextEmpty,   // an empty `old` would match everywhere; refused up front
};

struct ApplyResult {
    bool    ok = false;      // the mutation was applied (implies !inert)
    Inert   inert = Inert::No;
    QString patched;         // valid iff ok
    int     occurrences = 0; // how many times `old` appeared
};

// Apply ONE mutation to `content`. Every occurrence of `oldText` is replaced,
// which is deliberate: a caller mutating "the constant 3" means all of them,
// and a unique-match rule would refuse the common case. `occurrences` is
// reported so a caller can see it hit more than it expected.
ApplyResult applyOne(const QString &content, const Mutation &m);

// Pass/fail counts read out of a test runner's output. -1 means "not found",
// which is distinct from 0 and must stay so: a run whose output could not be
// parsed has not told you that nothing passed.
struct Counts {
    int passed = -1;
    int failed = -1;
};

// Recognises the pytest summary line ("3 failed, 5 passed in 1.2s"), ctest's
// ("100% tests passed, 0 tests failed out of 42"), and gtest's
// ("[  PASSED  ] 5 tests." / "[  FAILED  ] 2 tests"). Anything else leaves
// both at -1 rather than guessing.
Counts parseCounts(const QString &output);

// ANTS-4401 — what `require_green_baseline` decides, as a pure function of
// what the baseline run reported.
//
// It exists because the gate used to test for RED (timed out, or a non-zero
// exit) and treat everything else as green. Two states are neither: an
// unparsable summary, which ANTS-4398 deliberately reports as -1 rather than 0
// because "a run whose output could not be read has NOT said that nothing
// passed"; and a run that executed nothing, which a gtest binary under a
// filter matching no test reports as 0/0 with exit 0. Both satisfied a gate
// whose entire job is to refuse an unproven baseline.
enum class BaselineVerdict {
    Green,       // ran, passed something, failed nothing
    NotGreen,    // timed out or exited non-zero
    Unreadable,  // exit 0, counts unparsable (-1)
    Empty,       // exit 0, parsed, nothing ran (0/0)
};
BaselineVerdict judgeBaseline(bool timedOut, int exitCode, const Counts &c);

}  // namespace MutationProbe
