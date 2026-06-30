// Feature-conformance test for spec.md (ANTS-2227, part 1).
//
// Behavioural tests on ants::falsepos::appendEntry(dryRun) (a free function),
// plus source-scrapes for the per-handler + schema wiring (the handlers need a
// full RemoteControl to run, so they are verified by source-scrape — the same
// posture as tests/features/mcp_apply_edits).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "falseposledger.h"

#include <QString>
#include <QTemporaryDir>
#include <QFileInfo>
#include <string>

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

size_t countOf(const std::string &hay, const std::string &needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

ants::falsepos::LedgerEntry validEntry() {
    ants::falsepos::LedgerEntry e;
    e.reviewKind = QStringLiteral("audit");
    e.claim      = QStringLiteral("foo.cpp:10 flagged as unused but is a slot");
    e.rationale  = QStringLiteral("connected via QMetaObject; tool can't see it");
    e.timestamp  = QStringLiteral("2026-06-30");
    e.loggedBy   = QStringLiteral("cc-session");
    return e;
}

}  // namespace

// INV-1 + INV-2 — dry_run leaves the ledger absent, and the would-be byte
// count equals what a subsequent real append writes.
TEST(McpDryRunParity, FalseposDryRunNoWriteThenRealMatches) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QString ledger = root + QStringLiteral("/.ants_review_falsepos.jsonl");

    const auto dry = ants::falsepos::appendEntry(root, validEntry(), /*dryRun=*/true);
    EXPECT_TRUE(dry.ok) << dry.message.toStdString();
    EXPECT_TRUE(dry.created);             // file absent → would create
    EXPECT_GT(dry.bytesAppended, 0);
    EXPECT_FALSE(QFileInfo::exists(ledger))
        << "dry_run must not create the ledger file";

    const auto real = ants::falsepos::appendEntry(root, validEntry(), /*dryRun=*/false);
    EXPECT_TRUE(real.ok) << real.message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(ledger)) << "real append must create the ledger";
    EXPECT_EQ(dry.bytesAppended, real.bytesAppended)
        << "dry_run preview must match the real write (shared code path)";
}

// INV-3 — the preview still runs full validation.
TEST(McpDryRunParity, FalseposDryRunValidationStillFires) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants::falsepos::LedgerEntry bad = validEntry();
    bad.reviewKind = QStringLiteral("not-a-kind");
    const auto r = ants::falsepos::appendEntry(tmp.path(), bad, /*dryRun=*/true);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_args"));
    EXPECT_FALSE(QFileInfo::exists(
        tmp.path() + QStringLiteral("/.ants_review_falsepos.jsonl")));
}

// INV-4 — per-handler dry_run gates (source-scrape).
TEST(McpDryRunParity, HandlerGatesWired) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());

    // apply_edits — reads dry_run and has a would-write branch before QSaveFile.
    EXPECT_TRUE(has(rc, "const bool dryRun = req.value(QStringLiteral(\"dry_run\")).toBool();"));
    EXPECT_TRUE(has(rc, "if (dryRun) env[\"dry_run\"] = true;"))
        << "apply_edits envelope must carry dry_run:true";
    // project_settings — writeOut dry_run early-return.
    EXPECT_TRUE(has(rc, "o[QStringLiteral(\"would_write\")] = true;"))
        << "project_settings writeOut must preview would_write";
    // feedback_log — pre-write preview envelope.
    EXPECT_TRUE(has(rc, "out[\"bytes_appended\"] = static_cast<qint64>(addedUtf8.size());"));
    // audit_falsepos_log — passes the flag to appendEntry.
    EXPECT_TRUE(has(rc, "ants::falsepos::appendEntry(root, e, dryRun)"))
        << "audit_falsepos_log must thread dry_run into appendEntry";
}

// INV-5 — uniform schema prop factory, declared on all four descriptors.
TEST(McpDryRunParity, SchemaPropWired) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_TRUE(has(ci, "auto makeDryRunProp = []"))
        << "makeDryRunProp factory missing";
    EXPECT_GE(countOf(ci, "= makeDryRunProp();"), 4u)
        << "dry_run prop must be declared on all four part-1 descriptors";
}
