// ANTS-1856 — feature-conformance test for the roadmap_query `id`
// single-item selector. Source-scrape style matching the sibling
// ANTS-1425 (narrator_filter) / ANTS-1436 (pagination) tests: the id
// branch lives inside cmdRoadmapQuery, which the feature bundle does
// not link, so the contract is locked via anchor scrapes across the
// three wiring files (handler / schema / dispatch).

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
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

// Index of the first occurrence (npos if absent) — used for ordering
// assertions (INV-2: id branch precedes status filter / pagination).
size_t at(const std::string &hay, const std::string &needle) {
    return hay.find(needle);
}

}  // namespace

// INV-1 — id read from req with 64-byte + control-char hygiene.
TEST(roadmap_query_by_id, Inv1IdReadAndHygiene) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1856"),
           "INV-1: ANTS-1856 anchor present in cmdRoadmapQuery");
    expect(contains(cpp, "req.value(QStringLiteral(\"id\"))"),
           "INV-1: cmdRoadmapQuery reads id from req");
    expect(contains(cpp, "idArg.truncate(64)"),
           "INV-1: id echo truncated to 64 bytes");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — id branch precedes the status filter + pagination so an id
// fetch bypasses both.
TEST(roadmap_query_by_id, Inv2BypassesStatusAndPagination) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const size_t idBranch = at(cpp, "if (!idArg.isEmpty()) {");
    const size_t statusFilter = at(cpp, "ANTS-1247-INV-2/3");
    expect(idBranch != std::string::npos,
           "INV-2: single-item id branch present");
    expect(statusFilter != std::string::npos,
           "INV-2: status-filter anchor present");
    expect(idBranch != std::string::npos &&
               statusFilter != std::string::npos &&
               idBranch < statusFilter,
           "INV-2: id branch must run BEFORE the status filter so an "
           "id fetch returns the item regardless of lifecycle and is "
           "never paginated");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — case-only mismatch surfaces bad_case + canonical_id.
TEST(roadmap_query_by_id, Inv3CaseMismatchBadCase) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "Qt::CaseInsensitive"),
           "INV-3: case-insensitive fallback scan present");
    expect(contains(cpp, "out[\"canonical_id\"]"),
           "INV-3: canonical_id surfaced on case mismatch");
    expect(contains(cpp, "\"bad_case\""),
           "INV-3: bad_case code on case-only mismatch");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — unknown id is ok:true with found:false, not an error.
TEST(roadmap_query_by_id, Inv4UnknownIdFoundFalse) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "out[\"found\"]   = !matches.isEmpty()") ||
               contains(cpp, "out[\"found\"] = !matches.isEmpty()"),
           "INV-4: found reflects whether the id matched");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — id + section and id + section_index both rejected.
TEST(roadmap_query_by_id, Inv5CombosRejected) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp,
                    "id selector does not combine with "
                    "mode:section_index"),
           "INV-5: id + section_index → bad_mode_combo");
    expect(contains(cpp, "id selector searches the whole roadmap"),
           "INV-5: id + section → bad_mode_combo");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — body kept by default; stripped only on explicit opt-out.
TEST(roadmap_query_by_id, Inv6BodyDefaultOn) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp,
                    "if (hasIncludeBodyArg && !includeBody) "
                    "rcStripBodyFields(matches)"),
           "INV-6: id fetch keeps body unless include_body:false is "
           "explicitly passed");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — schema advertises the id property.
TEST(roadmap_query_by_id, Inv7SchemaAdvertisesId) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "props[\"id\"] = idProp"),
           "INV-7: id property registered on roadmap_query schema");
    expect(contains(cpp, "ANTS-1856"),
           "INV-7: ANTS-1856 anchor in claudeintegration");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — dispatch forwards id to the handler. ANTS-3422 replaced the
// per-arg forward with a verbatim `rcDelegate` forward that passes the
// whole args object to cmdRoadmapQuery, so `id` reaches the handler by
// construction (it takes the list path on an empty/absent id).
TEST(roadmap_query_by_id, Inv8DispatchForwardsId) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "rcDelegate(&RemoteControl::cmdRoadmapQuery)"),
           "INV-8: roadmap_query registered via the verbatim rcDelegate "
           "forward, so id reaches the handler (not silently dropped)");
    expect(contains(cpp, "ANTS-3422"),
           "INV-8: ANTS-3422 verbatim-forward anchor present");
    EXPECT_EQ(0, expect_failures());
}
