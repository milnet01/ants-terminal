// Feature-conformance test for the audit_falsepos_log MCP tool (ANTS-2129).
// Drives RemoteControl::cmdAuditFalseposLog (m_main-independent) against a
// QTemporaryDir ledger, and round-trips through ants::falsepos::loadEntries.
// See spec.md + docs/specs/ANTS-2129.md.

#include "falseposledger.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDate>
#include <QFile>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>  // ::symlink

namespace {

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString ledgerPath(const QTemporaryDir &d) {
    return d.path() + QStringLiteral("/.ants_review_falsepos.jsonl");
}

// Minimal valid request against `dir`.
QJsonObject baseReq(const QTemporaryDir &dir) {
    QJsonObject r;
    r["caller_cwd"]  = dir.path();
    r["review_kind"] = "audit";
    r["claim"]       = "cppcheck flags I_Error as non-noreturn";
    r["rationale"]   = "I_Error is _Noreturn (C11); cppcheck doesn't model it.";
    return r;
}

}  // namespace

// INV-8 — absent ledger is created (created:true, mode 0644); loadEntries
// returns the one written entry.
TEST(AuditFalseposLog, CreatesLedger) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdAuditFalseposLog(baseReq(dir)).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "create should succeed";
    EXPECT_TRUE(env.value("created").toBool());
    EXPECT_EQ(env.value("review_kind").toString(), "audit");
    EXPECT_FALSE(env.value("timestamp").toString().isEmpty());

    const QString p = ledgerPath(dir);
    ASSERT_TRUE(QFile::exists(p));
    // Mode 0644 on create (true Unix mode via stat — Qt's permission flags
    // don't map to octal bits directly).
    struct stat st {};
    ASSERT_EQ(::stat(p.toUtf8().constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644) << "ledger should be created 0644";

    const auto entries = ants::falsepos::loadEntries(dir.path());
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().reviewKind, "audit");
}

// INV-11 — success envelope shape + bytes_appended == file byte growth
// (single-writer temp dir).
TEST(AuditFalseposLog, EnvelopeAndByteCount) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    const QString p = ledgerPath(dir);
    const qint64 before = readAll(p).size();  // 0 (absent)
    const QJsonObject env = rc.cmdAuditFalseposLog(baseReq(dir)).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.contains("path"));
    EXPECT_TRUE(env.contains("created"));
    EXPECT_TRUE(env.contains("timestamp"));
    EXPECT_TRUE(env.contains("review_kind"));
    const qint64 appended = env.value("bytes_appended").toInteger();
    const qint64 after = readAll(p).size();
    EXPECT_GT(appended, 0);
    EXPECT_EQ(after - before, appended);
}

// INV-1 — append preserves prior bytes; new record is a strict suffix.
TEST(AuditFalseposLog, AppendIsStrictSuffix) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    const QString p = ledgerPath(dir);

    ASSERT_TRUE(rc.cmdAuditFalseposLog(baseReq(dir)).object()
                    .value("ok").toBool());
    const QByteArray afterA = readAll(p);

    QJsonObject reqB = baseReq(dir);
    reqB["claim"] = "second finding";
    const QJsonObject envB = rc.cmdAuditFalseposLog(reqB).object();
    ASSERT_TRUE(envB.value("ok").toBool());
    const QByteArray afterB = readAll(p);

    EXPECT_TRUE(afterB.startsWith(afterA)) << "prior bytes must be preserved";
    EXPECT_EQ(afterB.size() - afterA.size(),
              envB.value("bytes_appended").toInteger());

    const auto entries = ants::falsepos::loadEntries(dir.path());
    EXPECT_EQ(entries.size(), 2);
}

// INV-2 — leading/trailing newline self-heals a torn (un-terminated) tail.
TEST(AuditFalseposLog, SelfHealsTornTail) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = ledgerPath(dir);
    // Seed a valid record followed by a torn partial line (no trailing \n).
    {
        QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("\n{\"review_kind\":\"audit\",\"claim\":\"c\","
                "\"rationale\":\"r\",\"timestamp\":\"2026-06-15\"}\n");
        f.write("{\"review_kind\":\"audit\",\"claim\":\"torn");  // no newline
        f.close();
    }
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdAuditFalseposLog(baseReq(dir)).object();
    ASSERT_TRUE(env.value("ok").toBool());
    // The seeded valid record + the new one parse; the torn line is skipped.
    const auto entries = ants::falsepos::loadEntries(dir.path());
    EXPECT_EQ(entries.size(), 2);
    // The leading \n of the new record terminated the orphan line.
    const QByteArray on = readAll(p);
    EXPECT_TRUE(on.contains("\"torn\n") || on.contains("torn\n"))
        << "leading newline must terminate the orphan";
}

// INV-3 — embedded newline in claim/rationale/lane is JSON-escaped; one line.
TEST(AuditFalseposLog, EmbeddedNewlineEscaped) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(dir);
    req["rationale"] = "line one\nline two";
    req["lane"]      = "auth\ninjected";
    ASSERT_TRUE(rc.cmdAuditFalseposLog(req).object().value("ok").toBool());

    // Exactly one JSON line (the record): file is "\n"+json+"\n", so splitting
    // on '\n' yields one non-empty physical line.
    const QByteArray on = readAll(ledgerPath(dir));
    int nonEmpty = 0;
    for (const QByteArray &ln : on.split('\n'))
        if (!ln.trimmed().isEmpty()) ++nonEmpty;
    EXPECT_EQ(nonEmpty, 1) << "record must be a single physical line";

    const auto entries = ants::falsepos::loadEntries(dir.path());
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().rationale, "line one\nline two");
}

// INV-12 — full round-trip of all fields (control-char-free tags).
TEST(AuditFalseposLog, RoundTripFields) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(dir);
    req["review_kind"] = "indie-review";
    req["claim"]       = "missing rate-limit on /login";
    req["rationale"]   = "enforced at nginx; see infra/nginx.conf";
    req["lane"]        = "auth";
    req["topic"]       = "rate-limit";
    req["logged_by"]   = "user-confirmed";
    req["timestamp"]   = "2026-06-15";
    ASSERT_TRUE(rc.cmdAuditFalseposLog(req).object().value("ok").toBool());

    const auto entries = ants::falsepos::loadEntries(dir.path());
    ASSERT_EQ(entries.size(), 1);
    const auto &e = entries.first();
    EXPECT_EQ(e.reviewKind, "indie-review");
    EXPECT_EQ(e.claim, "missing rate-limit on /login");
    EXPECT_EQ(e.rationale, "enforced at nginx; see infra/nginx.conf");
    EXPECT_EQ(e.lane, "auth");
    EXPECT_EQ(e.topic, "rate-limit");
    EXPECT_EQ(e.timestamp, "2026-06-15");
    EXPECT_EQ(e.loggedBy, "user-confirmed");
}

// INV-5 — timestamp defaults to today; malformed present value refuses.
TEST(AuditFalseposLog, TimestampDefaultAndValidation) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    // Default: absent → today.
    const QJsonObject env = rc.cmdAuditFalseposLog(baseReq(dir)).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("timestamp").toString(),
              QDate::currentDate().toString("yyyy-MM-dd"));
    // Malformed present → bad_args.
    QJsonObject bad = baseReq(dir);
    bad["timestamp"] = "2026-13-40";
    EXPECT_EQ(rc.cmdAuditFalseposLog(bad).object().value("code").toString(),
              "bad_args");
}

// INV-6 — over-cap rationale trimmed to <= 1024 sliced units + ellipsis.
TEST(AuditFalseposLog, RationaleTrimmed) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(dir);
    req["rationale"] = QString(2000, QChar('a'));  // 2000 ASCII units
    ASSERT_TRUE(rc.cmdAuditFalseposLog(req).object().value("ok").toBool());
    const auto entries = ants::falsepos::loadEntries(dir.path());
    ASSERT_EQ(entries.size(), 1);
    const QString r = entries.first().rationale;
    EXPECT_TRUE(r.endsWith(QStringLiteral("…")));
    // sliced portion <= 1024 (assert <=, never ==): total <= 1025.
    EXPECT_LE(r.size(), 1025);
    EXPECT_GT(r.size(), 1000);
}

// INV-7 — multibyte record over 3.5 KiB after trim refuses; file untouched.
TEST(AuditFalseposLog, OverSizeRecordRefused) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(dir);
    // 1024 + 280 CJK units (3 UTF-8 bytes each) -> > 3584 B after trim.
    req["rationale"] = QString(1024, QChar(0x4E00));
    req["claim"]     = QString(280, QChar(0x4E00));
    const QJsonObject env = rc.cmdAuditFalseposLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "bad_args");
    EXPECT_FALSE(QFile::exists(ledgerPath(dir)))
        << "an over-size first write must not create the ledger";

    // All-ASCII same-shape control: well under the bound, succeeds.
    QJsonObject ok = baseReq(dir);
    ok["rationale"] = QString(1024, QChar('a'));
    ok["claim"]     = QString(280, QChar('a'));
    EXPECT_TRUE(rc.cmdAuditFalseposLog(ok).object().value("ok").toBool());
}

// INV-4 / INV-10 / no_project — refusals.
TEST(AuditFalseposLog, Refusals) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);

    auto code = [&](const QJsonObject &r) {
        return rc.cmdAuditFalseposLog(r).object().value("code").toString();
    };

    // INV-4 — empty claim / rationale.
    { QJsonObject r = baseReq(dir); r["claim"] = ""; EXPECT_EQ(code(r), "bad_args"); }
    { QJsonObject r = baseReq(dir); r["rationale"] = ""; EXPECT_EQ(code(r), "bad_args"); }
    // INV-4 — absent claim.
    { QJsonObject r = baseReq(dir); r.remove("claim"); EXPECT_EQ(code(r), "bad_args"); }
    // INV-10 — non-canonical / empty / absent review_kind.
    { QJsonObject r = baseReq(dir); r["review_kind"] = "frobnicate";
      EXPECT_EQ(code(r), "bad_args"); }
    // ANTS-3701 — "debt-sweep" is canonical, so it must NOT refuse. Its own
    // temp dir on purpose: an accepted call writes the ledger, and this test's
    // closing assertion is that none of the refusals above created one.
    { QTemporaryDir accepted; ASSERT_TRUE(accepted.isValid());
      QJsonObject r = baseReq(accepted); r["review_kind"] = "debt-sweep";
      EXPECT_NE(code(r), "bad_args"); }
    { QJsonObject r = baseReq(dir); r["review_kind"] = "";
      EXPECT_EQ(code(r), "bad_args"); }
    { QJsonObject r = baseReq(dir); r.remove("review_kind");
      EXPECT_EQ(code(r), "bad_args"); }
    // no_project — caller_cwd is not an existing directory.
    { QJsonObject r = baseReq(dir);
      r["caller_cwd"] = dir.path() + "/does-not-exist";
      EXPECT_EQ(code(r), "no_project"); }

    // None of the refusals created the ledger.
    EXPECT_FALSE(QFile::exists(ledgerPath(dir)));
}

// INV-9 — non-regular ledger path (directory, then symlink) → write_failed.
TEST(AuditFalseposLog, NonRegularPathRefused) {
    // (a) directory at the ledger path.
    {
        QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkdir(".ants_review_falsepos.jsonl"));
        RemoteControl rc(nullptr);
        EXPECT_EQ(rc.cmdAuditFalseposLog(baseReq(dir)).object()
                      .value("code").toString(),
                  "write_failed");
    }
    // (b) symlink at the ledger path → refuse; target untouched.
    {
        QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
        const QString target = dir.path() + "/outside.txt";
        { QFile f(target); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
          f.write("orig"); f.close(); }
        ASSERT_EQ(::symlink(target.toUtf8().constData(),
                            ledgerPath(dir).toUtf8().constData()), 0);
        RemoteControl rc(nullptr);
        EXPECT_EQ(rc.cmdAuditFalseposLog(baseReq(dir)).object()
                      .value("code").toString(),
                  "write_failed");
        EXPECT_EQ(readAll(target), QByteArray("orig"))
            << "symlink target must not be written through";
    }
}
