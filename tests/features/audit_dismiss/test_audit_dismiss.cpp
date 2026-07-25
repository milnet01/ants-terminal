// Feature-conformance test for tests/features/audit_dismiss/spec.md.
//
// ANTS-1713 — `audit_dismiss` MCP verb: the write side of the
// fingerprint-keyed learned-FP ledger. Drives RemoteControl directly with a
// null MainWindow (the seam mcp_roadmap_log_atomicity established).

#include "auditfpledger.h"
#include "auditrunner.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

// A cppcheck-shaped finding: the line-based parser keys its check id on the
// tool name, which is what `rule` must be for these detectors.
const char *kRaw =
    "src/foo.cpp:42:7: warning: bogus thing here [uselessAssignment]\n";
const char *kRule = "cppcheck";
const char *kFile = "src/foo.cpp";
const char *kMessage =
    "src/foo.cpp:42:7: warning: bogus thing here [uselessAssignment]";

QJsonObject dismissReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("rule")]       = QString::fromLatin1(kRule);
    o[QStringLiteral("file")]       = QString::fromLatin1(kFile);
    o[QStringLiteral("message")]    = QString::fromLatin1(kMessage);
    o[QStringLiteral("reason")]     = QStringLiteral("fixture, not a real bug");
    return o;
}

QString ledgerPath(const QString &root) {
    return root + QStringLiteral("/.audit_cache/learned-fp.jsonl");
}

}  // namespace

// INV-1 — file+message form: the server computes the fingerprint and appends.
TEST(AuditDismiss, Inv1ComputesFingerprintFromFileAndMessage) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    const QJsonObject resp = rc.cmdAuditDismiss(dismissReq(tmp.path())).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("computed")).toBool())
        << "INV-1: server-side hash must be reported as computed";

    const QString fp = resp.value(QStringLiteral("fingerprint")).toString();
    EXPECT_EQ(fp, ants::auditfp::computeFingerprint(kFile, kRule, kMessage))
        << "INV-1: must be the same hash the engine looks up";
    EXPECT_TRUE(QFile::exists(ledgerPath(tmp.path())))
        << "INV-1: entry lands in .audit_cache/learned-fp.jsonl";

    const QList<ants::auditfp::Entry> loaded =
        ants::auditfp::loadEntries(tmp.path());
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.first().fingerprint, fp);
    EXPECT_EQ(loaded.first().rule, QString::fromLatin1(kRule));
    EXPECT_FALSE(loaded.first().timestamp.isEmpty());
}

// INV-2 — explicit fingerprint accepted; a malformed one refuses.
TEST(AuditDismiss, Inv2ExplicitFingerprint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")]  = tmp.path();
    req[QStringLiteral("rule")]        = QString::fromLatin1(kRule);
    req[QStringLiteral("fingerprint")] = QStringLiteral("00112233445566aa");
    const QJsonObject ok = rc.cmdAuditDismiss(req).object();
    EXPECT_TRUE(ok.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(ok.value(QStringLiteral("computed")).toBool())
        << "INV-2: a caller-supplied hash is not 'computed'";
    EXPECT_EQ(ok.value(QStringLiteral("fingerprint")).toString(),
              QStringLiteral("00112233445566aa"));

    for (const QString &bad : {QStringLiteral("NOTHEX"),
                               QStringLiteral("00112233445566AA"),
                               QStringLiteral("0011223344")}) {
        req[QStringLiteral("fingerprint")] = bad;
        const QJsonObject r = rc.cmdAuditDismiss(req).object();
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"))
            << "INV-2: malformed fingerprint \"" << bad.toStdString()
            << "\" must refuse rather than write an unmatchable entry";
    }
}

// INV-3 — required inputs enforced.
TEST(AuditDismiss, Inv3RequiredInputs) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject noRule = dismissReq(tmp.path());
    noRule[QStringLiteral("rule")] = QStringLiteral("   ");
    EXPECT_EQ(rc.cmdAuditDismiss(noRule).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args")) << "INV-3: blank rule refuses";

    QJsonObject noIdent;
    noIdent[QStringLiteral("caller_cwd")] = tmp.path();
    noIdent[QStringLiteral("rule")]       = QString::fromLatin1(kRule);
    EXPECT_EQ(rc.cmdAuditDismiss(noIdent).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"))
        << "INV-3: neither fingerprint nor file+message refuses";

    QJsonObject badRoot = dismissReq(tmp.path());
    badRoot[QStringLiteral("caller_cwd")] =
        tmp.path() + QStringLiteral("/no/such/dir");
    EXPECT_EQ(rc.cmdAuditDismiss(badRoot).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("no_project"))
        << "INV-3: unresolvable caller_cwd refuses no_project";
}

// INV-4 — dry_run validates and reports but writes nothing.
TEST(AuditDismiss, Inv4DryRunWritesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req = dismissReq(tmp.path());
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = rc.cmdAuditDismiss(req).object();
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("fingerprint")).toString(),
              ants::auditfp::computeFingerprint(kFile, kRule, kMessage))
        << "INV-4: dry_run still reports the real hash";
    EXPECT_FALSE(QFile::exists(ledgerPath(tmp.path())))
        << "INV-4: dry_run must not create the ledger";
}

// INV-5 — end-to-end: after the dismissal, the headless parse path (the one
// runAudit uses) actually drops the finding.
TEST(AuditDismiss, Inv5DismissalSuppressesInHeadlessParse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    const QSet<QString> before = ants::auditfp::fingerprintSet(
        ants::auditfp::loadEntries(tmp.path()));
    const AuditRunner::internal::ParsedCounts pre =
        AuditRunner::internal::parseWithSuppression(kRule, kRaw, 10, before);
    ASSERT_EQ(pre.rawCount, 1) << "fixture must parse as one finding";
    ASSERT_EQ(pre.afterFilterCount, 1) << "control: not suppressed yet";

    ASSERT_TRUE(rc.cmdAuditDismiss(dismissReq(tmp.path())).object()
                    .value(QStringLiteral("ok")).toBool());

    const QSet<QString> after = ants::auditfp::fingerprintSet(
        ants::auditfp::loadEntries(tmp.path()));
    const AuditRunner::internal::ParsedCounts post =
        AuditRunner::internal::parseWithSuppression(kRule, kRaw, 10, after);
    EXPECT_EQ(post.rawCount, 1)
        << "INV-5: rawCount keeps the tool's true raw total";
    EXPECT_EQ(post.afterFilterCount, 0)
        << "INV-5: the dismissed finding must actually drop";
    EXPECT_EQ(post.sampleCount, 0) << "INV-5: and must not be sampled";
}

// INV-6 — re-dismissing the same finding is a no-op, not an error.
TEST(AuditDismiss, Inv6ReDismissIsIdempotent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    ASSERT_TRUE(rc.cmdAuditDismiss(dismissReq(tmp.path())).object()
                    .value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(rc.cmdAuditDismiss(dismissReq(tmp.path())).object()
                    .value(QStringLiteral("ok")).toBool())
        << "INV-6: a repeat dismissal must not be an error";
    EXPECT_EQ(ants::auditfp::loadEntries(tmp.path()).size(), 1)
        << "INV-6: and must not duplicate the entry";
}
