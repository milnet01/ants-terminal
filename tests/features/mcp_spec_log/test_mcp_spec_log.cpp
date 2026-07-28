// Feature-conformance test for the spec_log MCP tool (ANTS-1963).
// Drives the pure SpecLog transforms directly + cmdSpecLog (caller_cwd
// canonical root, m_main-independent). See spec.md + docs/specs/ANTS-1963.md.

#include "speclog.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Seed a project root with docs/specs/<id>.md containing `body`.
QString seedSpec(const QTemporaryDir &dir, const QString &id,
                 const QString &body) {
    const QString rel = "docs/specs";
    QDir(dir.path()).mkpath(rel);
    const QString path = dir.path() + "/" + rel + "/" + id + ".md";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body.toUtf8());
    f.close();
    return path;
}

const char *kSpec =
    "# ANTS-9999 — Sample spec\n"
    "\n"
    "**Status:** spec draft (2026-06-03).\n"
    "**Kind:** implement.\n"
    "\n"
    "## 3. Invariants\n"
    "\n"
    "- **INV-1** — first invariant. *Test:* T1.\n"
    "- **INV-2** — second invariant. *Test:* T2.\n"
    "\n"
    "## 8. Cold-eyes loop log\n"
    "\n"
    "- **Loop 1 (2026-06-03)** — initial pass.\n";

}  // namespace

// T1 (pure) — set_status rewrites only the Status line.
TEST(McpSpecLog, SetStatusPure) {
    const SpecLog::EditResult r =
        SpecLog::setStatus(QString::fromUtf8(kSpec),
                           QStringLiteral("accepted (2026-06-04)"));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.content.contains("**Status:** accepted (2026-06-04)"));
    EXPECT_FALSE(r.content.contains("spec draft"));
    EXPECT_TRUE(r.content.contains("**Kind:** implement."));  // untouched
    EXPECT_GT(r.line, 0);
}

// T1 (refusal) — no Status line → unrecognised_format.
TEST(McpSpecLog, SetStatusNoLine) {
    const SpecLog::EditResult r =
        SpecLog::setStatus(QStringLiteral("# Title\n\nbody only.\n"),
                           QStringLiteral("accepted"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, "unrecognised_format");
}

// T3 (pure) — append_inv appends at the end of Invariants, with *Test:*.
TEST(McpSpecLog, AppendInvPure) {
    const SpecLog::EditResult r =
        SpecLog::appendInv(QString::fromUtf8(kSpec), QStringLiteral("INV-3"),
                           QStringLiteral("third invariant"),
                           QStringLiteral("T3"));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.content.contains(
        "- **INV-3** — third invariant. *Test:* T3."));
    // Existing INVs unchanged + not renumbered.
    EXPECT_TRUE(r.content.contains("- **INV-1** — first invariant."));
    EXPECT_TRUE(r.content.contains("- **INV-2** — second invariant."));
    // New bullet sits inside the Invariants section, before loop log.
    const int invIdx = r.content.indexOf("**INV-3**");
    const int logIdx = r.content.indexOf("Cold-eyes loop log");
    EXPECT_GT(invIdx, 0);
    EXPECT_LT(invIdx, logIdx) << "INV-3 must land before the next section";
}

// T4 — duplicate / malformed inv_id → bad_args.
TEST(McpSpecLog, AppendInvRefusals) {
    const SpecLog::EditResult dup =
        SpecLog::appendInv(QString::fromUtf8(kSpec), QStringLiteral("INV-2"),
                           QStringLiteral("dup"), QString());
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(dup.code, "bad_args");

    // No Invariants section → unrecognised_format.
    const SpecLog::EditResult none =
        SpecLog::appendInv(QStringLiteral("# T\n\nbody.\n"),
                           QStringLiteral("INV-1"),
                           QStringLiteral("x"), QString());
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(none.code, "unrecognised_format");
}

// T2 (pure) — append_loop appends at the end of an existing section.
TEST(McpSpecLog, AppendLoopPure) {
    const SpecLog::EditResult r =
        SpecLog::appendLoop(QString::fromUtf8(kSpec),
                            QStringLiteral("Loop 2 (2026-06-04)"),
                            QStringLiteral("second pass, clean."));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.content.contains(
        "- **Loop 2 (2026-06-04)** — second pass, clean."));
    EXPECT_TRUE(r.content.contains("- **Loop 1 (2026-06-03)** — initial"));
    EXPECT_GT(r.line, 0);
}

// T5 — append_loop on a spec with no loop-log section creates it at EOF.
TEST(McpSpecLog, AppendLoopCreatesSection) {
    const QString noLog =
        QStringLiteral("# T\n\n**Status:** draft.\n\n## 3. Invariants\n\n"
                       "- **INV-1** — x. *Test:* T1.\n");
    const SpecLog::EditResult r =
        SpecLog::appendLoop(noLog, QStringLiteral("Loop 1 (2026-06-04)"),
                            QStringLiteral("first."));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.content.contains("## Cold-eyes loop log"));
    EXPECT_TRUE(r.content.contains("- **Loop 1 (2026-06-04)** — first."));
    // Heading + bullet at EOF, after the Invariants section.
    EXPECT_GT(r.content.indexOf("Cold-eyes loop log"),
              r.content.indexOf("Invariants"));
}

// T8 — fenced `## ` inside a section is not a premature boundary.
TEST(McpSpecLog, FencedHeadingSafety) {
    const QString spec = QStringLiteral(
        "# T\n\n**Status:** d.\n\n## 3. Invariants\n\n"
        "- **INV-1** — x. *Test:* T1.\n\n"
        "```\n## Not A Section\n```\n\n"
        "- **INV-2** — y. *Test:* T2.\n\n"
        "## 8. Cold-eyes loop log\n\n"
        "- **Loop 1** — p.\n");
    const SpecLog::EditResult r =
        SpecLog::appendInv(spec, QStringLiteral("INV-3"),
                           QStringLiteral("z"), QStringLiteral("T3"));
    ASSERT_TRUE(r.ok);
    // INV-3 must land after INV-2 (the section did NOT end at the fenced
    // "## Not A Section") and before the loop-log section.
    const int inv2 = r.content.indexOf("**INV-2**");
    const int inv3 = r.content.indexOf("**INV-3**");
    const int log  = r.content.indexOf("Cold-eyes loop log");
    EXPECT_GT(inv3, inv2);
    EXPECT_LT(inv3, log);
}

// T1/T10 (handler) — set_status via cmdSpecLog routes by id + envelope.
TEST(McpSpecLog, HandlerSetStatus) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedSpec(dir, "ANTS-9999", QString::fromUtf8(kSpec));
    ASSERT_FALSE(p.isEmpty());

    const qint64 sizeBefore = QFileInfo(p).size();

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["id"] = "ANTS-9999";
    req["op"] = "set_status";
    req["status"] = "accepted (2026-06-04)";
    const QJsonObject env = rc.cmdSpecLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "set_status should succeed";
    EXPECT_EQ(env.value("op").toString(), "set_status");
    EXPECT_EQ(env.value("id").toString(), "ANTS-9999");
    EXPECT_EQ(env.value("path").toString(), "docs/specs/ANTS-9999.md");
    EXPECT_GT(env.value("line").toInt(), 0);
    EXPECT_TRUE(QString::fromUtf8(readAll(p)).contains(
        "**Status:** accepted (2026-06-04)"));

    // ANTS-3724 — bytes_written is the ADDED-bytes delta, file_bytes the whole
    // file (parity with roadmap_log/changelog_log). The old `bytes_written > 0`
    // assertion could not distinguish the two, and is wrong here besides: this
    // set_status swaps a longer status for a shorter one, so the honest delta
    // is NEGATIVE. Pin both against the file on disk.
    const qint64 sizeAfter = QFileInfo(p).size();
    EXPECT_EQ(env.value("file_bytes").toInteger(), sizeAfter);
    EXPECT_EQ(env.value("bytes_written").toInteger(), sizeAfter - sizeBefore);
    EXPECT_LT(env.value("bytes_written").toInteger(), 0)
        << "shorter replacement status must report a negative delta, not the "
           "whole-file size";
}

// T6 — refusals.
TEST(McpSpecLog, Refusals) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir, "ANTS-9999", QString::fromUtf8(kSpec));
    RemoteControl rc(nullptr);
    auto base = [&]() {
        QJsonObject r; r["caller_cwd"] = dir.path(); return r;
    };

    // bad op → bad_mode (id valid + file present).
    { QJsonObject r = base(); r["id"] = "ANTS-9999"; r["op"] = "weird";
      EXPECT_EQ(rc.cmdSpecLog(r).object().value("code").toString(),
                "bad_mode"); }
    // malformed id → bad_id.
    { QJsonObject r = base(); r["id"] = "NOTANID"; r["op"] = "set_status";
      r["status"] = "x";
      EXPECT_EQ(rc.cmdSpecLog(r).object().value("code").toString(),
                "bad_id"); }
    // path traversal → bad_path.
    { QJsonObject r = base(); r["path"] = "../x"; r["op"] = "set_status";
      r["status"] = "x";
      EXPECT_EQ(rc.cmdSpecLog(r).object().value("code").toString(),
                "bad_path"); }
    // neither id nor path → bad_id.
    { QJsonObject r = base(); r["op"] = "set_status"; r["status"] = "x";
      EXPECT_EQ(rc.cmdSpecLog(r).object().value("code").toString(),
                "bad_id"); }
    // absent spec file → not_found (distinct from unrecognised_format).
    { QJsonObject r = base(); r["id"] = "ANTS-1234"; r["op"] = "set_status";
      r["status"] = "x";
      EXPECT_EQ(rc.cmdSpecLog(r).object().value("code").toString(),
                "not_found"); }
}

// T7 — atomicity: forced write failure leaves the spec byte-identical.
TEST(McpSpecLog, AtomicWriteFailure) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedSpec(dir, "ANTS-9999", QString::fromUtf8(kSpec));
    const QByteArray before = readAll(p);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = dir.path(); req["id"] = "ANTS-9999";
    req["op"] = "set_status"; req["status"] = "accepted";

    RemoteControl::setForceSpecWriteFailForTest(true);
    const QJsonObject env = rc.cmdSpecLog(req).object();
    RemoteControl::setForceSpecWriteFailForTest(false);

    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "write_failed");
    EXPECT_EQ(readAll(p), before) << "spec must be byte-identical";
}

// T11 (ANTS-2136) — dry_run previews the landing line + bytes without
// writing the spec, for each of the three section-routed ops.
TEST(McpSpecLog, HandlerDryRunWritesNothing) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seedSpec(dir, "ANTS-9999", QString::fromUtf8(kSpec));
    const QByteArray before = readAll(p);

    RemoteControl rc(nullptr);
    auto base = [&]() {
        QJsonObject r; r["caller_cwd"] = dir.path(); r["id"] = "ANTS-9999";
        r["dry_run"] = true; return r;
    };

    // set_status dry_run.
    { QJsonObject r = base(); r["op"] = "set_status";
      r["status"] = "accepted (2026-06-17)";
      const QJsonObject env = rc.cmdSpecLog(r).object();
      ASSERT_TRUE(env.value("ok").toBool())
          << env.value("error").toString().toStdString();
      EXPECT_TRUE(env.value("dry_run").toBool());
      EXPECT_GT(env.value("line").toInt(), 0);
      EXPECT_GT(env.value("bytes").toInt(), 0);
      EXPECT_FALSE(env.contains("bytes_written"));
      // ANTS-3724 — the write-side pair is absent on a preview, both halves.
      EXPECT_FALSE(env.contains("file_bytes")); }

    // append_inv dry_run.
    { QJsonObject r = base(); r["op"] = "append_inv";
      r["inv_id"] = "INV-3"; r["body"] = "third invariant";
      const QJsonObject env = rc.cmdSpecLog(r).object();
      ASSERT_TRUE(env.value("ok").toBool())
          << env.value("error").toString().toStdString();
      EXPECT_TRUE(env.value("dry_run").toBool()); }

    // append_loop dry_run.
    { QJsonObject r = base(); r["op"] = "append_loop";
      r["loop_label"] = "Loop 2 (2026-06-17)"; r["body"] = "second pass.";
      const QJsonObject env = rc.cmdSpecLog(r).object();
      ASSERT_TRUE(env.value("ok").toBool())
          << env.value("error").toString().toStdString();
      EXPECT_TRUE(env.value("dry_run").toBool()); }

    // None of the three previews touched the spec.
    EXPECT_EQ(readAll(p), before) << "dry_run must not write the spec";
}
