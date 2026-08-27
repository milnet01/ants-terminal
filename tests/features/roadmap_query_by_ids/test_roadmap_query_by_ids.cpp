// ANTS-1726 — feature-conformance test for the roadmap_query `ids`
// plural-item selector. Source-scrape style matching ANTS-1856 (its
// singular sibling): cmdRoadmapQuery is not linked by the feature
// bundle, so the contract is locked via anchor scrapes across the
// three wiring files (handler / schema / dispatch).

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

size_t at(const std::string &hay, const std::string &needle) {
    return hay.find(needle);
}

}  // namespace

// INV-1 — ids read from req as a JSON array with per-element hygiene.
TEST(roadmap_query_by_ids, Inv1IdsReadAndHygiene) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1726"),
           "INV-1: ANTS-1726 anchor present in cmdRoadmapQuery");
    expect(contains(cpp, "req.value(QStringLiteral(\"ids\"))"),
           "INV-1: cmdRoadmapQuery reads ids from req");
    expect(contains(cpp, "idsArg"),
           "INV-1: idsArg variable named");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — ids branch precedes status filter + pagination so an
// ids fetch bypasses both, returning bullets in document order.
TEST(roadmap_query_by_ids, Inv2BypassesStatusAndPagination) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // ANTS-4724 — the anchor must be UNIQUE to the ids branch.
    // `if (!idsArg.isEmpty())` is not: it is a substring of an
    // `else if (!idsArg.isEmpty())` in the `query` bad_mode_combo chain
    // far earlier in the file. A window opened there measures a position
    // that has nothing to do with the ids selector, and the ordering
    // assert below then holds wherever the real branch sits.
    // `wanted(idsArg.cbegin()` occurs once, inside the branch.
    const size_t idsBranch    = at(cpp, "wanted(idsArg.cbegin()");
    const size_t statusFilter = at(cpp, "ANTS-1247-INV-2/3");
    expect(idsBranch != std::string::npos,
           "INV-2: multi-item ids branch present");
    expect(statusFilter != std::string::npos,
           "INV-2: status-filter anchor present");
    expect(idsBranch != std::string::npos &&
               statusFilter != std::string::npos &&
               idsBranch < statusFilter,
           "INV-2: ids branch must run BEFORE the status filter");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — envelope carries matched_ids[] and missing_ids[] so a caller
// can spot unknown / stale ids without diffing.
TEST(roadmap_query_by_ids, Inv3MatchedAndMissingAccounting) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "out[\"matched_ids\"]"),
           "INV-3: matched_ids emitted in ids branch");
    expect(contains(cpp, "out[\"missing_ids\"]"),
           "INV-3: missing_ids emitted in ids branch");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — ids + id, ids + section, ids + section_index all rejected.
TEST(roadmap_query_by_ids, Inv4CombosRejected) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp,
                    "ids selector does not combine with id"),
           "INV-4: ids + id → bad_mode_combo");
    expect(contains(cpp,
                    "ids selector does not combine with section"),
           "INV-4: ids + section → bad_mode_combo");
    expect(contains(cpp,
                    "ids selector does not combine with "
                    "mode:section_index"),
           "INV-4: ids + section_index → bad_mode_combo");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — body kept by default in ids branch; stripped only on
// explicit include_body:false.
TEST(roadmap_query_by_ids, Inv5BodyDefaultOn) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // Same lambda phrasing as singular id branch (ANTS-1856 INV-6);
    // grep is shape-tolerant.
    expect(contains(cpp,
                    "if (hasIncludeBodyArg && !includeBody) "
                    "rcStripBodyFields(matches)"),
           "INV-5: ids fetch keeps body unless include_body:false");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — schema advertises the ids property as type:array.
TEST(roadmap_query_by_ids, Inv6SchemaAdvertisesIds) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "props[\"ids\"] = idsProp"),
           "INV-6: ids property registered on roadmap_query schema");
    expect(contains(cpp, "ANTS-1726"),
           "INV-6: ANTS-1726 anchor in claudeintegration");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — dispatch forwards ids to the handler. ANTS-3422 replaced the
// per-arg forward with a verbatim `rcDelegate` forward that passes the
// whole args object to cmdRoadmapQuery, so `ids` reaches the handler by
// construction (an empty/absent array falls through to the list path).
TEST(roadmap_query_by_ids, Inv7DispatchForwardsIds) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "rcDelegate(&RemoteControl::cmdRoadmapQuery)"),
           "INV-7: roadmap_query registered via the verbatim rcDelegate "
           "forward, so ids reach the handler (not silently dropped)");
    expect(contains(cpp, "ANTS-3422"),
           "INV-7: ANTS-3422 verbatim-forward anchor present");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — input list capped at 100 elements; oversize → bad_args.
TEST(roadmap_query_by_ids, Inv10ArrayCap) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ids array must contain at most 100"),
           "INV-10: oversize ids array refuses bad_args");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3541 INV-11 — a comma/whitespace-joined STRING `ids` is coerced
// to the array form instead of silently falling through to the full
// list (the mistyped-scalar footgun). Source anchor: the isString()
// coercion branch + the [,\s]+ split in cmdRoadmapQuery.
TEST(roadmap_query_by_ids, Inv11StringIdsCoerced) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-3541"),
           "INV-11: ANTS-3541 anchor present in cmdRoadmapQuery");
    expect(contains(cpp, "idsVal.isString()"),
           "INV-11: string ids coerced (isString branch)");
    expect(contains(cpp, "raw.split(") &&
               contains(cpp, "Qt::SkipEmptyParts"),
           "INV-11: comma/whitespace split on the coerced string");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3541 INV-12 — a present-but-invalid `ids` refuses bad_args
// rather than returning a large wrong result: (a) a non-array/
// non-string scalar, and (b) a NON-EMPTY ids yielding zero valid ids
// after hygiene. Empty array / empty string keep the documented
// absent fall-through (INV-1).
TEST(roadmap_query_by_ids, Inv12PresentButInvalidRefuses) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "got a non-string scalar"),
           "INV-12: non-array/non-string ids refuses bad_args");
    expect(contains(cpp, "ids contained no valid id after hygiene"),
           "INV-12: non-empty-but-all-malformed ids refuses bad_args");
    expect(contains(cpp, "idsPresentNonEmpty"),
           "INV-12: non-empty tracking guards the documented "
           "empty-array/empty-string fall-through");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-4712 INV-13 — the `ids` branch is ANTS-1881 INV-2's one
// documented exemption: under mode:"headline_only" it emits the four
// projected keys PLUS `input_index`. The exemption is produced by an
// ORDERING — the projection runs first, then ANTS-4400's annotation
// writes the fifth key back on — so the ordering is what this locks.
// Reversing it would strip the key silently, reintroducing the
// mis-pairing ANTS-4400 closed for exactly the leanest caller.
// Scoped to the ids branch: both the projection call and the
// annotation loop are searched from the branch opening onward, since
// the singular `id` branch above carries its own projection call and
// takes no annotation.
TEST(roadmap_query_by_ids, Inv2IdsBranchKeepsInputIndex) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The anchor must be UNIQUE to the ids branch. `if (!idsArg.isEmpty())`
    // is not: it is a substring of an `else if (!idsArg.isEmpty())` far
    // earlier in the file, so a window opened there starts ABOVE the
    // singular `id` branch and picks up ITS projection call — which sits
    // before the annotation whichever way the ids branch is ordered, so the
    // ordering assert below holds vacuously. Caught by mutating the ordering
    // and watching this test pass anyway. `wanted(idsArg.cbegin()` occurs
    // once, inside the branch, above both sites being compared.
    const size_t idsBranch = at(cpp, "const QSet<QString> wanted(idsArg.cbegin()");
    expect(idsBranch != std::string::npos,
           "INV-13: multi-item ids branch present (unique anchor)");
    if (idsBranch == std::string::npos) {
        EXPECT_EQ(0, expect_failures());
        return;
    }
    const size_t projPos  = cpp.find("rcProjectHeadlineOnly(matches)", idsBranch);
    const size_t annotPos = cpp.find("input_index", idsBranch);
    expect(projPos != std::string::npos,
           "INV-13: ids branch projects under mode:\"headline_only\"");
    expect(annotPos != std::string::npos,
           "INV-13: ids branch annotates each bullet with input_index");
    expect(projPos != std::string::npos && annotPos != std::string::npos &&
               projPos < annotPos,
           "INV-13: the projection must run BEFORE the input_index "
           "annotation — reversing it strips the key the exemption exists "
           "to keep");
    // The annotation must not be gated on mode: it is owed on every
    // ids fetch, and a mode guard here is the shape a well-meaning
    // four-key repair would take.
    expect(!contains(cpp.substr(annotPos, 200), "headline_only"),
           "INV-13: the input_index annotation is not gated on mode");
    EXPECT_EQ(0, expect_failures());
}
