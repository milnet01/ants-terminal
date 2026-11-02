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
#include <QJsonArray>
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

QJsonObject byMessage(const char *file, const char *message) {
    QJsonObject o;
    o[QStringLiteral("file")]    = QString::fromLatin1(file);
    o[QStringLiteral("message")] = QString::fromLatin1(message);
    return o;
}

// Rows on disk, which loadEntries folds by fingerprint and so cannot count.
int dataRowCount(const QString &root) {
    QFile f(ledgerPath(root));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    int rows = 0;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (line.startsWith('{')) ++rows;
    return rows;
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

// INV-6 — the repeat says what happened, so a dropped reason is not silent.
TEST(AuditDismiss, Inv6RepeatReportsTheStoredReason) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    const QJsonObject first = rc.cmdAuditDismiss(dismissReq(tmp.path())).object();
    EXPECT_EQ(first.value(QStringLiteral("outcome")).toString(),
              QStringLiteral("recorded"));
    EXPECT_FALSE(first.contains(QStringLiteral("hint")));

    QJsonObject again = dismissReq(tmp.path());
    const QJsonObject same = rc.cmdAuditDismiss(again).object();
    EXPECT_EQ(same.value(QStringLiteral("outcome")).toString(),
              QStringLiteral("already_recorded"));
    EXPECT_FALSE(same.contains(QStringLiteral("hint")))
        << "INV-6: the same reason again has nothing to warn about";

    again[QStringLiteral("reason")] = QStringLiteral("a different reason");
    const QJsonObject differs = rc.cmdAuditDismiss(again).object();
    EXPECT_EQ(differs.value(QStringLiteral("outcome")).toString(),
              QStringLiteral("already_recorded"));
    EXPECT_EQ(differs.value(QStringLiteral("reason")).toString(),
              QStringLiteral("fixture, not a real bug"))
        << "INV-6: reports the reason the ledger holds, not the caller's";
    EXPECT_TRUE(differs.value(QStringLiteral("hint")).toString()
                    .contains(QStringLiteral("replace_reason")));
}

// INV-7 — many findings in one call, with the call's values as fallbacks.
TEST(AuditDismiss, Inv7EntriesRecordMany) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("rule")]       = QStringLiteral("vulture");
    req[QStringLiteral("reason")]     = QStringLiteral("route handler");
    req[QStringLiteral("entries")]    = QJsonArray{
        byMessage("app.py", "unused function 'index'"),
        byMessage("app.py", "unused function 'login'"),
        QJsonObject{{QStringLiteral("fingerprint"),
                     QStringLiteral("00112233445566aa")},
                    {QStringLiteral("rule"), QStringLiteral("typos")},
                    {QStringLiteral("reason"), QStringLiteral("own word")}},
    };

    const QJsonObject resp = rc.cmdAuditDismiss(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 3);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 0);

    const QJsonArray applied = resp.value(QStringLiteral("applied")).toArray();
    ASSERT_EQ(applied.size(), 3);
    EXPECT_EQ(applied.at(1).toObject().value(QStringLiteral("index")).toInt(), 1);
    EXPECT_EQ(applied.at(1).toObject().value(QStringLiteral("fingerprint")).toString(),
              ants::auditfp::computeFingerprint(
                  "app.py", "vulture", "unused function 'login'"))
        << "INV-7: the call's rule goes into the hash";

    const QList<ants::auditfp::Entry> loaded =
        ants::auditfp::loadEntries(tmp.path());
    ASSERT_EQ(loaded.size(), 3);
    EXPECT_EQ(loaded.at(0).reason, QStringLiteral("route handler"));
    EXPECT_EQ(loaded.at(2).rule, QStringLiteral("typos"))
        << "INV-7: an entry's own rule beats the call's";
    EXPECT_EQ(loaded.at(2).reason, QStringLiteral("own word"));
}

// INV-7 — the same finding twice in one batch is one row.
TEST(AuditDismiss, Inv7RepeatInsideABatchIsOneRow) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("rule")]       = QStringLiteral("vulture");
    req[QStringLiteral("entries")]    = QJsonArray{
        byMessage("app.py", "unused function 'index'"),
        byMessage("app.py", "unused function 'index'"),
    };
    const QJsonArray applied =
        rc.cmdAuditDismiss(req).object().value(QStringLiteral("applied")).toArray();
    ASSERT_EQ(applied.size(), 2);
    EXPECT_EQ(applied.at(1).toObject().value(QStringLiteral("outcome")).toString(),
              QStringLiteral("already_recorded"));
    EXPECT_EQ(dataRowCount(tmp.path()), 1);
}

// INV-8 — a bad entry costs only itself; nothing applied is a refusal.
TEST(AuditDismiss, Inv8BadEntryLandsInSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("rule")]       = QStringLiteral("vulture");
    req[QStringLiteral("entries")]    = QJsonArray{
        QJsonObject{{QStringLiteral("fingerprint"), QStringLiteral("NOTHEX")}},
        byMessage("app.py", "unused function 'index'"),
    };
    const QJsonObject mixed = rc.cmdAuditDismiss(req).object();
    EXPECT_TRUE(mixed.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(mixed.value(QStringLiteral("applied_count")).toInt(), 1);
    const QJsonArray skipped = mixed.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("index")).toInt(), 0);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_EQ(dataRowCount(tmp.path()), 1);

    QTemporaryDir tmp2;
    ASSERT_TRUE(tmp2.isValid());
    req[QStringLiteral("caller_cwd")] = tmp2.path();
    req[QStringLiteral("entries")]    = QJsonArray{
        QJsonObject{{QStringLiteral("fingerprint"), QStringLiteral("NOTHEX")}},
    };
    const QJsonObject allBad = rc.cmdAuditDismiss(req).object();
    EXPECT_FALSE(allBad.value(QStringLiteral("ok")).toBool())
        << "INV-8: nothing applied must not read as success";
    EXPECT_EQ(allBad.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_EQ(allBad.value(QStringLiteral("skipped_count")).toInt(), 1);
    EXPECT_FALSE(QFile::exists(ledgerPath(tmp2.path())));
}

// INV-9 — one identity cannot apply to a whole batch, and entries must be real.
TEST(AuditDismiss, Inv9BatchRefusesASharedIdentityAndEmptyEntries) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject shared = dismissReq(tmp.path());
    shared[QStringLiteral("entries")] =
        QJsonArray{byMessage("app.py", "unused function 'index'")};
    EXPECT_EQ(rc.cmdAuditDismiss(shared).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("rule")]       = QStringLiteral("vulture");
    req[QStringLiteral("entries")]    = QJsonArray{};
    EXPECT_EQ(rc.cmdAuditDismiss(req).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args")) << "INV-9: empty entries";
    req[QStringLiteral("entries")] = QStringLiteral("not an array");
    EXPECT_EQ(rc.cmdAuditDismiss(req).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args")) << "INV-9: non-array entries";
    EXPECT_FALSE(QFile::exists(ledgerPath(tmp.path())));
}

// INV-10 — a wrong reason can be corrected through the verb.
TEST(AuditDismiss, Inv10ReplaceReason) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdAuditDismiss(dismissReq(tmp.path())).object()
                    .value(QStringLiteral("ok")).toBool());

    QJsonObject replace = dismissReq(tmp.path());
    replace[QStringLiteral("reason")]         = QStringLiteral("the right reason");
    replace[QStringLiteral("replace_reason")] = true;
    const QJsonObject resp = rc.cmdAuditDismiss(replace).object();
    EXPECT_EQ(resp.value(QStringLiteral("outcome")).toString(),
              QStringLiteral("reason_replaced"));
    EXPECT_EQ(resp.value(QStringLiteral("reason")).toString(),
              QStringLiteral("the right reason"));

    const QList<ants::auditfp::Entry> loaded =
        ants::auditfp::loadEntries(tmp.path());
    ASSERT_EQ(loaded.size(), 1) << "INV-10: still one finding";
    EXPECT_EQ(loaded.first().reason, QStringLiteral("the right reason"));
    EXPECT_EQ(dataRowCount(tmp.path()), 2) << "INV-10: the old row stays on disk";

    EXPECT_EQ(rc.cmdAuditDismiss(replace).object()
                  .value(QStringLiteral("outcome")).toString(),
              QStringLiteral("already_recorded"));
    EXPECT_EQ(dataRowCount(tmp.path()), 2)
        << "INV-10: the same reason again writes nothing";

    QJsonObject noReason = replace;
    noReason.remove(QStringLiteral("reason"));
    EXPECT_EQ(rc.cmdAuditDismiss(noReason).object()
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"))
        << "INV-10: replacing needs a reason to replace with";
}

// INV-10 — the correction keeps the stored rule, and records a new finding.
TEST(AuditDismiss, Inv10ReplaceKeepsRuleAndRecordsAnUnknownFinding) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")]     = tmp.path();
    req[QStringLiteral("rule")]           = QStringLiteral("first_rule");
    req[QStringLiteral("fingerprint")]    = QStringLiteral("00112233445566aa");
    req[QStringLiteral("reason")]         = QStringLiteral("first");
    req[QStringLiteral("replace_reason")] = true;
    EXPECT_EQ(rc.cmdAuditDismiss(req).object()
                  .value(QStringLiteral("outcome")).toString(),
              QStringLiteral("recorded"));

    req[QStringLiteral("rule")]   = QStringLiteral("other_rule");
    req[QStringLiteral("reason")] = QStringLiteral("second");
    ASSERT_EQ(rc.cmdAuditDismiss(req).object()
                  .value(QStringLiteral("outcome")).toString(),
              QStringLiteral("reason_replaced"));
    const QList<ants::auditfp::Entry> loaded =
        ants::auditfp::loadEntries(tmp.path());
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.first().rule, QStringLiteral("first_rule"));
    EXPECT_EQ(loaded.first().reason, QStringLiteral("second"));
}

// INV-11 — dry_run predicts both new forms and leaves the bytes alone.
TEST(AuditDismiss, Inv11DryRunCoversBatchAndReplace) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdAuditDismiss(dismissReq(tmp.path())).object()
                    .value(QStringLiteral("ok")).toBool());
    QFile ledger(ledgerPath(tmp.path()));
    ASSERT_TRUE(ledger.open(QIODevice::ReadOnly));
    const QByteArray before = ledger.readAll();
    ledger.close();

    QJsonObject replaceOne;
    replaceOne[QStringLiteral("file")]           = QString::fromLatin1(kFile);
    replaceOne[QStringLiteral("message")]        = QString::fromLatin1(kMessage);
    replaceOne[QStringLiteral("reason")]         = QStringLiteral("corrected");
    replaceOne[QStringLiteral("replace_reason")] = true;

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("rule")]       = QString::fromLatin1(kRule);
    req[QStringLiteral("dry_run")]    = true;
    req[QStringLiteral("entries")]    = QJsonArray{
        replaceOne, byMessage("app.py", "unused function 'index'")};

    const QJsonObject resp = rc.cmdAuditDismiss(req).object();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    const QJsonArray applied = resp.value(QStringLiteral("applied")).toArray();
    ASSERT_EQ(applied.size(), 2);
    EXPECT_EQ(applied.at(0).toObject().value(QStringLiteral("outcome")).toString(),
              QStringLiteral("reason_replaced"));
    EXPECT_EQ(applied.at(1).toObject().value(QStringLiteral("outcome")).toString(),
              QStringLiteral("recorded"));

    ASSERT_TRUE(ledger.open(QIODevice::ReadOnly));
    EXPECT_EQ(ledger.readAll(), before) << "INV-11: dry_run wrote to the ledger";
}
