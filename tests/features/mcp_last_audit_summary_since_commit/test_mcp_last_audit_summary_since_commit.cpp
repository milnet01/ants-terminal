// Source-grep harness for ANTS-1406 — locks the wiring contract for
// the `since_commit` short-circuit parameter on `last_audit_summary`.
// See spec.md.
//
// Exit 0 = all 8 invariants hold.

#include "../../_support/expect.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Source-grep helpers: locate the cmdLastAuditSummary body in
// remotecontrol.cpp so subsequent assertions only consider that
// function. The body extends from the symbol definition to the
// next function definition that follows it.
std::string lasSummaryBody(const std::string &rcCpp) {
    const size_t start =
        rcCpp.find("RemoteControl::cmdLastAuditSummary(");
    if (start == std::string::npos) return {};
    // Take a generous window — the function is ~250 lines.
    const size_t end = std::min(rcCpp.size(), start + 16000);
    return rcCpp.substr(start, end - start);
}

}  // namespace

TEST(McpLastAuditSummarySinceCommit, WiringContract) {
    expect_reset();

    const std::string ciCpp =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string lasBody = lasSummaryBody(rcCpp);

    // Use a single large window so the schema-builder block and
    // the property declarations (which sit ~6-9 KB further down)
    // are both visible from the lasTool registration anchor. The
    // next sibling tool (`current_state`, ANTS-1569) sits even
    // further down so we don't risk crossing tool boundaries.
    const size_t lasPos =
        ciCpp.find("lasTool[\"name\"] = \"last_audit_summary\"");
    expect(lasPos != std::string::npos,
           "INV-1 location",
           "tools/list missing last_audit_summary name registration");
    std::string lasWindow;
    if (lasPos != std::string::npos) {
        // Bounded by the next tool's name registration (or 12 KB).
        size_t windowEnd = ciCpp.find(
            "tools.append(lasTool)", lasPos);
        if (windowEnd == std::string::npos) {
            windowEnd = std::min(ciCpp.size(), lasPos + 12000);
        } else {
            windowEnd = std::min(ciCpp.size(), windowEnd + 200);
        }
        lasWindow = ciCpp.substr(lasPos, windowEnd - lasPos);
    }

    // INV-1 — since_commit property + sinceCommitProp builder.
    expect(contains(lasWindow, "\"since_commit\""),
           "INV-1",
           "last_audit_summary schema does not declare a "
           "\"since_commit\" property (ANTS-1406)");
    expect(contains(lasWindow, "sinceCommitProp"),
           "INV-1 prop-var",
           "schema builder missing sinceCommitProp QJsonObject "
           "(expected per ANTS-1406 wiring pattern)");

    // INV-2 — schema description names ANTS-1406 + `fresh` flag.
    expect(contains(lasWindow, "ANTS-1406"),
           "INV-2 anchor",
           "since_commit schema description does not cite "
           "ANTS-1406");
    expect(contains(lasWindow, "fresh:false") ||
           contains(lasWindow, "fresh:true"),
           "INV-2 fresh-flag",
           "schema description does not mention the `fresh` "
           "response field");

    // INV-3 — top-level tool description hints at since_commit.
    // The top-level description sits in the first ~3 KB of the
    // window so this is a tighter check.
    {
        const std::string topWindow =
            lasWindow.substr(0, std::min<size_t>(lasWindow.size(),
                                                 4000));
        expect(contains(topWindow, "since_commit"),
               "INV-3",
               "top-level last_audit_summary description does not "
               "surface the since_commit knob");
    }

    // INV-4 — the freshness-window constant exists and is 5 min.
    expect(contains(lasBody, "kSinceCommitFreshnessMs"),
           "INV-4 const-name",
           "cmdLastAuditSummary missing kSinceCommitFreshnessMs "
           "constant");
    expect(contains(lasBody, "5 * 60 * 1000"),
           "INV-4 const-value",
           "kSinceCommitFreshnessMs not literally `5 * 60 * 1000` "
           "(5-minute window per spec)");

    // INV-5 — three reason codes present in source.
    expect(contains(lasBody, "\"commit_drift\""),
           "INV-5 commit_drift",
           "cmdLastAuditSummary missing commit_drift reason code");
    expect(contains(lasBody, "\"stale_mtime\""),
           "INV-5 stale_mtime",
           "cmdLastAuditSummary missing stale_mtime reason code");
    expect(contains(lasBody, "\"no_provenance\""),
           "INV-5 no_provenance",
           "cmdLastAuditSummary missing no_provenance reason code");

    // INV-6 — last_run_age_ms field on the short envelope.
    expect(contains(lasBody, "last_run_age_ms"),
           "INV-6",
           "cmdLastAuditSummary short envelope missing "
           "last_run_age_ms field");

    // INV-7 — fresh:true flag on the success-pass.
    expect(contains(lasBody, "\"fresh\""),
           "INV-7",
           "cmdLastAuditSummary missing `fresh` flag on the full "
           "envelope (callers expect fresh:true when the gate "
           "lands on a cache hit)");

    // INV-8 — minimum match length enforced at 7 hex chars.
    expect(contains(lasBody, "n >= 7"),
           "INV-8",
           "cmdLastAuditSummary missing the `n >= 7` minimum-match "
           "guard (commit prefixes shorter than 7 are ambiguous)");

    EXPECT_EQ(0, expect_failures())
        << expect_failures()
        << " ANTS-1406 wiring invariant(s) failed";
}
