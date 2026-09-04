// Feature-conformance test for spec.md — ANTS-4444.
//
// Source-scrape, like its sibling audit_dedup_96bit: the filters live on
// AuditDialog, a QWidget whose suppression state is private, and the
// behaviour under test is which lookup each render path performs. Widening
// that state for a test would make an implementation detail a contract.
//
// INV-4 is not padding. The bulk edit that implements INV-3 also rewrites
// the parse-time cache line, which must NOT change — that mistake was made
// and caught while writing this fix, along with a self-recursive call the
// same edit produced.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_AUDITDIALOG_CPP_PATH
#error "SRC_AUDITDIALOG_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// The body of the Finding overload, bounded by the next function so an
// assertion about it cannot be met by a match elsewhere in the file.
std::string findingOverloadBody(const std::string &cpp) {
    const auto start = cpp.find("bool AuditDialog::isSuppressed(const Finding &f) const {");
    if (start == std::string::npos) return {};
    const auto end = cpp.find("bool AuditDialog::isSuppressed(const QString &", start);
    if (end == std::string::npos) return {};
    return cpp.substr(start, end - start);
}

size_t countOccurrences(const std::string &hay, const std::string &needle) {
    size_t n = 0;
    size_t p = hay.find(needle);
    while (p != std::string::npos) {
        ++n;
        p = hay.find(needle, p + needle.size());
    }
    return n;
}

}  // namespace

TEST(AuditLearnedFpFiltering, Ants4444LedgerIsConsulted) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "auditdialog.cpp not readable";

    const std::string body = findingOverloadBody(cpp);
    ASSERT_FALSE(body.empty())
        << "INV-1: isSuppressed(const Finding &) not found";

    // INV-1 — the ledger is actually consulted.
    expect(contains(body, "m_learnedFpFingerprints"),
           "INV-1: the Finding overload reads the learned-FP ledger");
    expect(contains(body, "computeFingerprint"),
           "INV-1: it keys the ledger by content fingerprint");

    // INV-2 — one definition of "suppressed by key".
    expect(contains(body, "isSuppressed(f.dedupKey)"),
           "INV-2: it delegates to the key lookup rather than repeating it");

    // INV-5 — an empty ledger must not cost a hash per finding per render.
    expect(contains(body, "m_learnedFpFingerprints.isEmpty()"),
           "INV-5: the empty ledger short-circuits before hashing");

    EXPECT_EQ(0, expect_failures());
}

TEST(AuditLearnedFpFiltering, Inv3FiltersTakeTheWholeFinding) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "auditdialog.cpp not readable";

    // INV-3 — no render/export filter may pass a bare key. That spelling is
    // exactly what made the ledger invisible to every user-facing path.
    //
    // Scoped to exclude the Finding overload's OWN body, where INV-2
    // requires that same spelling as the delegation. Unscoped, INV-2 and
    // INV-3 contradict each other and one of them must always fail; that is
    // how this read as a failure on a correct tree.
    const std::string overload = findingOverloadBody(cpp);
    ASSERT_FALSE(overload.empty())
        << "INV-3: isSuppressed(const Finding &) not found";
    std::string elsewhere = cpp;
    elsewhere.erase(elsewhere.find(overload), overload.size());

    expect(!contains(elsewhere, "if (isSuppressed(f.dedupKey))"),
           "INV-3: no filter passes a bare dedupKey; the ledger would be "
           "invisible to it");
    expect(countOccurrences(cpp, "if (isSuppressed(f))") >= 7,
           "INV-3: the render and export paths filter on the whole Finding");

    // INV-4 — but the parse-time cache keeps the key form. It runs BEFORE
    // applyLearnedFpSuppressions, so the Finding overload would hash for
    // nothing there.
    expect(contains(cpp, "f.suppressed = isSuppressed(f.dedupKey);"),
           "INV-4: the parse-time cache still primes from the key set");

    EXPECT_EQ(0, expect_failures());
}
