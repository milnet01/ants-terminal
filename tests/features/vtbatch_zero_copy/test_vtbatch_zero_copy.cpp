// Feature-conformance test for tests/features/vtbatch_zero_copy/spec.md.
//
// Source-grep harness — no Qt link. The behavioural contract (action-
// stream identity across the worker→GUI hop) is already pinned by
// tests/features/threaded_parse_equivalence/. This test locks the
// *shape* of the cross-thread signal so a future refactor can't
// silently revert the zero-copy by changing back to `const VtBatch &`.
//
// Exit 0 = all assertions hold.

#include <cstring>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int count(const std::string &hay, const char *needle) {
    int n = 0;
    size_t pos = 0;
    const size_t len = std::strlen(needle);
    if (len == 0) return 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += len;
    }
    return n;
}

void fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
}

}  // namespace

TEST(VtbatchZeroCopy, Main) {
    const std::string vtsHeader = ants_test::slurpFile(VTSTREAM_H);
    const std::string vtsSource = ants_test::slurpFile(VTSTREAM_CPP);
    const std::string twHeader = ants_test::slurpFile(TERMINALWIDGET_H);
    const std::string twSource = ants_test::slurpFile(TERMINALWIDGET_CPP);

    if (vtsHeader.empty()) fail("setup", "vtstream.h not readable");
    if (vtsSource.empty()) fail("setup", "vtstream.cpp not readable");
    if (twHeader.empty()) fail("setup", "terminalwidget.h not readable");
    if (twSource.empty()) fail("setup", "terminalwidget.cpp not readable");

    // I1 — Signal signature uses VtBatchPtr.
    if (!contains(vtsHeader, "void batchReady(VtBatchPtr batch)"))
        fail("I1", "signal does not take VtBatchPtr");
    if (contains(vtsHeader, "void batchReady(const VtBatch &"))
        fail("I1", "old `const VtBatch &` signal signature still present");

    // I2 — Alias declaration + Q_DECLARE_METATYPE.
    if (!contains(vtsHeader, "using VtBatchPtr = std::shared_ptr<const VtBatch>"))
        fail("I2", "VtBatchPtr alias missing");
    if (!contains(vtsHeader, "Q_DECLARE_METATYPE(VtBatchPtr)"))
        fail("I2", "Q_DECLARE_METATYPE(VtBatchPtr) missing");

    // I3 — Emitters use make_shared. Two emit sites: flushBatch and
    // onPtyFinished. Each constructs via make_shared<VtBatch>() and
    // populates via b->.
    if (count(vtsSource, "std::make_shared<VtBatch>()") < 2)
        fail("I3", "expected ≥2 std::make_shared<VtBatch>() emit-site builders");
    if (!contains(vtsSource, "emit batchReady(b)"))
        fail("I3", "emit batchReady(b) call site missing");
    // No stack-allocated VtBatch survives in the emit path.
    if (contains(vtsSource, "VtBatch b;\n    b.actions"))
        fail("I3", "old stack-VtBatch emit path still present");

    // I4 — Receiver dereferences via pointer.
    if (!contains(twHeader, "void onVtBatch(VtBatchPtr batch)"))
        fail("I4", "TerminalWidget::onVtBatch slot signature missing VtBatchPtr");
    if (contains(twHeader, "void onVtBatch(const VtBatch &"))
        fail("I4", "old `const VtBatch &` slot signature still present");
    if (!contains(twSource, "batch->actions"))
        fail("I4", "receiver does not dereference batch->actions");
    if (!contains(twSource, "batch->rawBytes"))
        fail("I4", "receiver does not dereference batch->rawBytes");
    // Old dot-access pattern must be gone (was `batch.actions`,
    // `batch.rawBytes`, `batch.clearSelectionHint`, `batch.wallClockMs`).
    if (contains(twSource, "batch.actions"))
        fail("I4", "stale `batch.actions` dot access remains");
    if (contains(twSource, "batch.rawBytes"))
        fail("I4", "stale `batch.rawBytes` dot access remains");

    // I6 — Metatype registered.
    if (!contains(vtsSource, "qRegisterMetaType<VtBatchPtr>(\"VtBatchPtr\")"))
        fail("I6", "qRegisterMetaType<VtBatchPtr>() missing");

    std::puts("OK vtbatch_zero_copy: 5/5 invariants (I5 covered by threaded_parse_equivalence)");
    return;
}

