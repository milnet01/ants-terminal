// ANTS-5057 — source-scrape regression test. See spec.md.
//
// Four invariants; INV-1..INV-3 are expected to fail against the current
// tree (the defect is still live), INV-4 is a guard expected to pass now
// and after the fix:
//   INV-1 — every DebtSweepEngine::scanAll( call sits inside a
//           QThread::create( worker lambda. Today the file has no
//           QThread::create( at all, so this fails outright.
//   INV-2 — onDebtScanClicked()'s body contains no processEvents. Today it
//           calls QApplication::processEvents() right before debtScan().
//   INV-3 — neither onDebtScanClicked() nor the fix/allow branches of
//           onDebtAnchorClicked() pair a synchronous debtScan() with an
//           immediately-following renderDebtResults() and nothing async
//           (QThread / connect() — this project's own worker idiom)
//           between the two calls. Today all three regions show that
//           pairing.
//   INV-4 (guard) — allowlisted(debtToAuditFinding( still appears in the
//           file; the threading fix must not drop the allowlist filter.
//
// AuditDialog is a QDialog; this project's house pattern for its
// invariants is source-scrape (see audit_dialog_render_hardening,
// audit_tool_process_group_kill, audit_blame_bulk_async), not
// construction. Every check below anchors on a QUALIFIED definition
// (e.g. "void AuditDialog::onDebtScanClicked(") rather than a bare name —
// a bare "onDebtScanClicked(" would match the connect(...,
// &AuditDialog::onDebtScanClicked) wiring in buildDebtSweepTab() first,
// which sits earlier in the file than the definition itself.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_AUDIT_CPP_PATH
#  error "SRC_AUDIT_CPP_PATH compile definition required"
#endif

namespace {

// INV-1 helper — true iff every "scanAll(" occurrence in `src` sits inside
// a QThread::create( lambda body: the nearest PRECEDING "QThread::create("
// must still have an unclosed brace scope (net '{' - '}' balance >= 1)
// at the point of the call. A brace-balance check rather than a fixed-byte
// window because the lambda body can grow without invalidating the check;
// it tolerates any correct implementation using the project's own
// QThread::create idiom without pinning a capture list or variable name.
// Records a diagnosable reason for the first violation found in `detail`.
bool allScanAllCallsInsideWorker(const std::string &src, std::string &detail,
                                  int &occurrences) {
    occurrences = 0;
    const std::string needle = "scanAll(";
    const std::string anchor = "QThread::create(";
    std::size_t pos = 0;
    while ((pos = src.find(needle, pos)) != std::string::npos) {
        ++occurrences;
        const std::size_t threadPos = src.rfind(anchor, pos);
        if (threadPos == std::string::npos) {
            detail = "scanAll( at byte offset " + std::to_string(pos) +
                      " has no preceding QThread::create( anywhere earlier "
                      "in the file — it runs on whatever thread calls it "
                      "directly, i.e. the GUI thread.";
            return false;
        }
        int depth = 0;
        for (std::size_t i = threadPos; i < pos; ++i) {
            if (src[i] == '{') ++depth;
            else if (src[i] == '}') --depth;
        }
        if (depth < 1) {
            detail = "scanAll( at byte offset " + std::to_string(pos) +
                      " follows a QThread::create( at offset " +
                      std::to_string(threadPos) + ", but the brace balance "
                      "between them is " + std::to_string(depth) +
                      " (<1) — the worker lambda (or some enclosing scope "
                      "opened after QThread::create() has already closed "
                      "before this call, so it is not running inside it.";
            return false;
        }
        pos += needle.size();
    }
    return true;
}

// INV-3 helper — true iff `region` shows the synchronous
// scan-then-render-on-the-spot idiom: a debtScan() call followed later in
// the same region by a renderDebtResults() call, with neither "QThread"
// nor "connect(" appearing anywhere between them. Those two substrings are
// the project's own async idiom (QThread::create(...) paired with a
// connect(...) to the worker's completion signal — see MainWindow,
// LuaEngine, cmdSessionOrient); their absence between the two calls means
// nothing interrupts the synchronous sequence, regardless of what
// intervening if/else branches or closing braces sit between them in
// source. Returns false (no defect found in this region) when either call
// is missing, so a refactor that renames one of the two calls away
// entirely is correctly NOT flagged — this check pins the *pairing*, not
// the presence of either name in isolation.
bool hasSyncScanThenRender(const std::string &region, std::string &between) {
    const std::size_t scanPos = region.find("debtScan()");
    if (scanPos == std::string::npos) return false;
    const std::size_t renderPos = region.find("renderDebtResults()", scanPos);
    if (renderPos == std::string::npos) return false;
    between = region.substr(scanPos, renderPos - scanPos);
    const bool hasAsyncMarker = between.find("QThread") != std::string::npos ||
                                 between.find("connect(") != std::string::npos;
    return !hasAsyncMarker;
}

}  // namespace

TEST(AuditDebtScanAsync, ScanAllOnlyCalledInsideWorker) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    std::string detail;
    int occurrences = 0;
    const bool ok = allScanAllCallsInsideWorker(stripped, detail, occurrences);

    ASSERT_GT(occurrences, 0)
        << "ANTS-5057: no DebtSweepEngine::scanAll( call found in "
           "auditdialog.cpp at all — has debtScan() moved elsewhere, or "
           "was scanAll( renamed? This test needs at least one call site "
           "to check.";

    EXPECT_TRUE(ok)
        << "ANTS-5057 INV-1: DebtSweepEngine::scanAll( must run only "
           "inside a QThread::create( worker lambda, never directly on "
           "the GUI thread. " << detail;
}

TEST(AuditDebtScanAsync, NoProcessEventsInScanClickHandler) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::onDebtScanClicked(");
    ASSERT_FALSE(body.empty())
        << "AuditDialog::onDebtScanClicked() body not found — anchor moved?";

    EXPECT_EQ(body.find("processEvents"), std::string::npos)
        << "ANTS-5057 INV-2: onDebtScanClicked() must not manually pump "
           "the event loop while waiting on the scan — its presence means "
           "the scan is still running synchronously on the GUI thread. "
           "Body: \"" << body << "\"";
}

TEST(AuditDebtScanAsync, NoSyncScanThenRenderInScanClickHandler) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::onDebtScanClicked(");
    ASSERT_FALSE(body.empty())
        << "AuditDialog::onDebtScanClicked() body not found — anchor moved?";

    std::string between;
    const bool synchronous = hasSyncScanThenRender(body, between);

    EXPECT_FALSE(synchronous)
        << "ANTS-5057 INV-3: onDebtScanClicked() pairs a synchronous "
           "debtScan() with an immediately-following renderDebtResults(), "
           "and nothing async (QThread / connect() — this project's "
           "worker idiom) sits between them. Text between the two calls: "
           "\"" << between << "\"";
}

TEST(AuditDebtScanAsync, NoSyncScanThenRenderInFixBranch) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::onDebtAnchorClicked(");
    ASSERT_FALSE(body.empty())
        << "AuditDialog::onDebtAnchorClicked() body not found — anchor moved?";

    const std::string fixRegion =
        ants_test::regionBetween(body, "ants-debt-fix", "ants-debt-allow");
    ASSERT_FALSE(fixRegion.empty())
        << "could not isolate the \"ants-debt-fix\" branch inside "
           "onDebtAnchorClicked() — has the scheme literal or branch order "
           "changed? Full body: \"" << body << "\"";

    std::string between;
    const bool synchronous = hasSyncScanThenRender(fixRegion, between);

    EXPECT_FALSE(synchronous)
        << "ANTS-5057 INV-3: the \"ants-debt-fix\" branch of "
           "onDebtAnchorClicked() pairs a synchronous debtScan() with an "
           "immediately-following renderDebtResults(), and nothing async "
           "(QThread / connect()) sits between them. Text between the two "
           "calls: \"" << between << "\"";
}

TEST(AuditDebtScanAsync, NoSyncScanThenRenderInAllowBranch) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::onDebtAnchorClicked(");
    ASSERT_FALSE(body.empty())
        << "AuditDialog::onDebtAnchorClicked() body not found — anchor moved?";

    const std::string allowRegion =
        ants_test::regionBetween(body, "ants-debt-allow", "ants-debt-defer");
    ASSERT_FALSE(allowRegion.empty())
        << "could not isolate the \"ants-debt-allow\" branch inside "
           "onDebtAnchorClicked() — has the scheme literal or branch order "
           "changed? Full body: \"" << body << "\"";

    std::string between;
    const bool synchronous = hasSyncScanThenRender(allowRegion, between);

    EXPECT_FALSE(synchronous)
        << "ANTS-5057 INV-3: the \"ants-debt-allow\" branch of "
           "onDebtAnchorClicked() pairs a synchronous debtScan() with an "
           "immediately-following renderDebtResults(), and nothing async "
           "(QThread / connect()) sits between them. Text between the two "
           "calls: \"" << between << "\"";
}

TEST(AuditDebtScanAsync, AllowlistFilterStillApplied) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    EXPECT_NE(stripped.find("allowlisted(debtToAuditFinding("), std::string::npos)
        << "ANTS-5057 INV-4 (guard): the allowlist filter "
           "allowlisted(debtToAuditFinding(...)) must still be applied to "
           "the debt scan result — the threading fix must not drop it "
           "while moving the scan off the GUI thread.";
}
