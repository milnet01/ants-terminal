// Feature-conformance test for the spec_log MCP tool (ANTS-1963).
// Drives the pure SpecLog transforms directly + cmdSpecLog (caller_cwd
// canonical root, m_main-independent). See spec.md + docs/specs/ANTS-1963.md.

#include "speclog.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
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

// ANTS-3785 — a WRAPPED Status field, the shape 49 of 172 specs in this
// corpus carry. Modelled on docs/specs/ANTS-3766-roadmap-migration-archives.md:
// opener + 3 continuation lines, terminated by `**Kind:**` with NO blank line,
// and an inline `**Split at loop 4:**` bold run inside the value (INV-9).
const char *kWrappedSpec =
    "# ANTS-9998 — Wrapped status spec\n"
    "\n"
    "**Status:** shipped (2026-08-01). **Split at loop 4:** the persistence "
    "half is\n"
    "[ANTS-3782](ANTS-3782-roadmap-section-provenance.md), which shipped in "
    "the\n"
    "same change; this document is the read half.\n"
    "**Kind:** implement.\n"
    "**Source:** ROADMAP.md ANTS-9998.\n"
    "\n"
    "## 1. Problem\n"
    "\n"
    "body.\n";

// Count of lines matching a prefix, for the orphan assertions below.
int countLinesStartingWith(const QString &text, const QString &prefix) {
    int n = 0;
    const auto lines = text.split(QLatin1Char('\n'));
    for (const QString &l : lines)
        if (l.startsWith(prefix)) ++n;
    return n;
}

}  // namespace

// ANTS-3785 INV-4 — setStatus replaces the field's WHOLE extent, so no
// continuation line survives the write. Against pre-fix code this FAILS:
// only the opener is replaced and the three continuation lines are orphaned
// as a stray paragraph, while the verb still reports ok.
TEST(McpSpecLog, SetStatusReplacesWholeWrappedExtent) {
    const SpecLog::EditResult r =
        SpecLog::setStatus(QString::fromUtf8(kWrappedSpec),
                           QStringLiteral("accepted (2026-08-02)"));
    ASSERT_TRUE(r.ok) << r.code.toStdString();

    EXPECT_EQ(countLinesStartingWith(r.content, QStringLiteral("**Status:**")), 1)
        << "exactly one Status line must survive";
    EXPECT_TRUE(r.content.contains("**Status:** accepted (2026-08-02)"));

    // The orphan signature: any continuation fragment left behind.
    EXPECT_FALSE(r.content.contains("[ANTS-3782]"))
        << "continuation line 1 orphaned";
    EXPECT_FALSE(r.content.contains("same change; this document is the read half."))
        << "continuation line 3 orphaned";
    EXPECT_FALSE(r.content.contains("Split at loop 4"))
        << "the inline bold run belonged to the replaced value";

    // **Kind:** is the very next line, with nothing wedged between.
    const auto lines = r.content.split(QLatin1Char('\n'));
    const int statusIdx = static_cast<int>(
        std::distance(lines.cbegin(),
                      std::find_if(lines.cbegin(), lines.cend(),
                                   [](const QString &l) {
                                       return l.startsWith("**Status:**");
                                   })));
    ASSERT_LT(statusIdx + 1, lines.size());
    EXPECT_EQ(lines.at(statusIdx + 1), QStringLiteral("**Kind:** implement."));

    // INV-5 — the returned line is 1-based and names the Status line.
    EXPECT_EQ(r.line, statusIdx + 1);
}

// ANTS-3785 INV-5 — bytes outside the field are untouched and the trailing
// newline is preserved exactly.
TEST(McpSpecLog, SetStatusWrappedPreservesSurroundingBytes) {
    const QString before = QString::fromUtf8(kWrappedSpec);
    const SpecLog::EditResult r =
        SpecLog::setStatus(before, QStringLiteral("accepted (2026-08-02)"));
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(r.content.startsWith("# ANTS-9998 — Wrapped status spec\n\n"))
        << "head bytes before the field must be identical";
    EXPECT_TRUE(r.content.endsWith("## 1. Problem\n\nbody.\n"))
        << "tail bytes after the field must be identical";
    EXPECT_TRUE(r.content.contains("**Source:** ROADMAP.md ANTS-9998."));
    EXPECT_TRUE(r.content.endsWith(QLatin1Char('\n')));
}

// ANTS-3785 INV-10 — the search is bounded to the header block, so a
// **Status:** line inside a fenced example below the first `## ` heading is
// never matched and the refusal still fires. Against an unbounded scan this
// FAILS: the fenced line is rewritten and the verb returns ok.
TEST(McpSpecLog, SetStatusIgnoresFencedStatusBelowHeaderBlock) {
    const QString doc = QStringLiteral(
        "# ANTS-9997 — No header status\n"
        "\n"
        "**Kind:** doc.\n"
        "\n"
        "## 3. Header block\n"
        "\n"
        "```\n"
        "**Status:** spec draft (YYYY-MM-DD).\n"
        "```\n");
    const SpecLog::EditResult r =
        SpecLog::setStatus(doc, QStringLiteral("accepted"));
    EXPECT_FALSE(r.ok) << "a fenced example is not this document's Status";
    EXPECT_EQ(r.code, "unrecognised_format");
}

// T1 (pure) — set_status rewrites the Status field and nothing else. (Its
// fixture's Status is single-line; the wrapped case is covered below.)
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

// T13 (ANTS-4114, pure) — set_status reports the value it REPLACED. The verb
// imposes no Status vocabulary (the caller's string is written verbatim), so
// the replaced value is the only evidence of the project's own vocabulary,
// and without it a wrong value surfaces at the project's lint gate minutes
// later instead of at the call.
TEST(McpSpecLog, SetStatusPureReportsPreviousValue) {
    const SpecLog::EditResult r =
        SpecLog::setStatus(QString::fromUtf8(kSpec),
                           QStringLiteral("accepted (2026-06-04)"));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.previousValue, QStringLiteral("spec draft (2026-06-03)."));
}

// T13 (ANTS-4114, pure) — a WRAPPED Status reports its whole extent, joined,
// not just the opener. Reporting the opener alone would misrepresent the
// vocabulary of exactly the specs (49 of 172) whose Status carries history.
TEST(McpSpecLog, SetStatusPreviousValueJoinsWrappedExtent) {
    const SpecLog::EditResult r =
        SpecLog::setStatus(QString::fromUtf8(kWrappedSpec),
                           QStringLiteral("accepted (2026-08-02)"));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.previousValue.startsWith(
        QStringLiteral("shipped (2026-08-01). **Split at loop 4:**")));
    EXPECT_TRUE(r.previousValue.endsWith(
        QStringLiteral("this document is the read half.")))
        << "the continuation lines belong to the replaced value";
    EXPECT_FALSE(r.previousValue.contains(QLatin1Char('\n')))
        << "the extent is space-joined, one line";
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

    // ANTS-4114 — the envelope names the value that was replaced, so a caller
    // writing a value the project's own standard rejects sees the mismatch
    // here rather than at the project's lint gate.
    EXPECT_EQ(env.value("previous_status").toString(),
              "spec draft (2026-06-03).");

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
      EXPECT_FALSE(env.contains("file_bytes"));
      // ANTS-4114 — previous_status is on the PREVIEW too: that is what makes
      // dry_run a free pre-flight for "what vocabulary does this project use?"
      // rather than something you learn only after writing.
      EXPECT_EQ(env.value("previous_status").toString(),
                "spec draft (2026-06-03)."); }

    // append_inv dry_run.
    { QJsonObject r = base(); r["op"] = "append_inv";
      r["inv_id"] = "INV-3"; r["body"] = "third invariant";
      const QJsonObject env = rc.cmdSpecLog(r).object();
      ASSERT_TRUE(env.value("ok").toBool())
          << env.value("error").toString().toStdString();
      EXPECT_TRUE(env.value("dry_run").toBool());
      // ANTS-4114 — set_status-only; an append op has no replaced value, and
      // an empty string would read as "the Status was blank".
      EXPECT_FALSE(env.contains("previous_status")); }

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

// ANTS-4364 — the shipped spec skeleton's loop log is a TABLE; this verb only
// ever wrote a bullet, so on a conforming spec it was unusable.
// `review-contract` therefore tells sessions outright not to reach for it,
// which is self-reinforcing: a verb the governing skill says to avoid gets no
// usage, so the mismatch never becomes pressure to fix.
//
// ANTS-4353 — and loop logs run in OPPOSITE directions across specs in one
// corpus. This project has both: ANTS-4108's spec runs oldest-first and
// ANTS-4065's runs newest-first, and a session prepended a row twice on a
// rule learned from the wrong sibling. A row at the wrong end reads as a
// different loop's result, and a checker that only balances per-row tallies
// passes the corruption — so the direction is INFERRED, not assumed.
TEST(McpSpecLog, Ants4364TableFormAnd4353DirectionInference) {
    const QString head = QStringLiteral(
        "# PROJ-1 — a spec\n"
        "\n"
        "## 12. Cold-eyes loop log\n"
        "\n"
        "| Loop | Date | Outcome |\n"
        "|------|------|---------|\n");

    // Oldest-first (1, 2) → the new row goes at the BOTTOM.
    const QString oldestFirst = head +
        QStringLiteral("| 1 | 2026-08-01 | 3 findings |\n"
                       "| 2 | 2026-08-02 | 1 finding |\n");
    const auto a = SpecLog::appendLoop(
        oldestFirst, QString(), QString(),
        {QStringLiteral("3"), QStringLiteral("2026-08-14"),
         QStringLiteral("0 findings")});
    ASSERT_TRUE(a.ok) << a.error.toStdString();
    EXPECT_EQ(a.rowShape, QStringLiteral("table"));
    EXPECT_EQ(a.rowOrder, QStringLiteral("oldest_first"));
    EXPECT_GT(a.content.indexOf(QStringLiteral("| 3 | 2026-08-14")),
              a.content.indexOf(QStringLiteral("| 2 | 2026-08-02")))
        << "an oldest-first log continues at the bottom";

    // Newest-first (2, 1) → the SAME call must go at the TOP. This is the
    // pair that makes the inference load-bearing: identical input, opposite
    // placement, decided only by the rows already there.
    const QString newestFirst = head +
        QStringLiteral("| 2 | 2026-08-02 | 1 finding |\n"
                       "| 1 | 2026-08-01 | 3 findings |\n");
    const auto b = SpecLog::appendLoop(
        newestFirst, QString(), QString(),
        {QStringLiteral("3"), QStringLiteral("2026-08-14"),
         QStringLiteral("0 findings")});
    ASSERT_TRUE(b.ok) << b.error.toStdString();
    EXPECT_EQ(b.rowOrder, QStringLiteral("newest_first"));
    EXPECT_LT(b.content.indexOf(QStringLiteral("| 3 | 2026-08-14")),
              b.content.indexOf(QStringLiteral("| 2 | 2026-08-02")))
        << "a newest-first log continues at the top — the opposite end, from "
           "the same call";

    // One row carries no direction. Reported as ambiguous rather than
    // guessed at silently.
    const QString single = head +
        QStringLiteral("| 1 | 2026-08-01 | 3 findings |\n");
    const auto c = SpecLog::appendLoop(
        single, QString(), QString(),
        {QStringLiteral("2"), QStringLiteral("2026-08-14"), QStringLiteral("x")});
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.rowOrder, QStringLiteral("ambiguous"));

    // A table with no `cells` REFUSES rather than writing a bullet into it —
    // that silent corruption is the original defect.
    const auto noCells =
        SpecLog::appendLoop(oldestFirst, QStringLiteral("3"),
                            QStringLiteral("0 findings"));
    EXPECT_FALSE(noCells.ok);
    EXPECT_EQ(noCells.code, QStringLiteral("format_mismatch"));
    EXPECT_TRUE(noCells.error.contains(QStringLiteral("Loop | Date | Outcome")))
        << "the refusal names the columns it read, so the caller can compose "
           "`cells` without opening the file";

    // Wrong column count refuses too, rather than writing a ragged row.
    const auto ragged = SpecLog::appendLoop(
        oldestFirst, QString(), QString(),
        {QStringLiteral("3"), QStringLiteral("2026-08-14")});
    EXPECT_FALSE(ragged.ok);
    EXPECT_EQ(ragged.code, QStringLiteral("column_mismatch"));

    // A BULLET loop log still takes the bullet form — the table support must
    // not have broken the shape that already worked.
    const QString bulletLog = QStringLiteral(
        "# PROJ-1 — a spec\n"
        "\n"
        "## 12. Cold-eyes loop log\n"
        "\n"
        "- **Loop 1** — 3 findings.\n");
    const auto d = SpecLog::appendLoop(bulletLog, QStringLiteral("Loop 2"),
                                       QStringLiteral("0 findings."));
    ASSERT_TRUE(d.ok) << d.error.toStdString();
    EXPECT_EQ(d.rowShape, QStringLiteral("bullet"));
    EXPECT_TRUE(d.content.contains(QStringLiteral("- **Loop 2** — 0 findings.")));
}

// ANTS-3651 — append_loop must append to the EXISTING loop-log section,
// wherever it sits, and must never create a second one.
//
// Observed on docs/specs/ANTS-3636.md: after appending loop 7 the file carried
// TWO `## Cold-eyes loop log` headings, loop 7 alone under the trailing one.
// Loops 8 and 9 then matched the FIRST, so the rendered order became
// 1-6, 8, 9, then 7 under a duplicate heading, and the repair was by hand.
//
// The absent-detection is what was wrong: the section was present and the verb
// created another anyway. `findSectionHeading` now scans the whole file for any
// level-2 heading carrying the keyword, so the section no longer has to be
// last. This locks that in — the fixture deliberately puts a section AFTER the
// loop log, which is the shape that used to fail.
TEST(McpSpecLog, Ants3651AppendsToLoopLogThatIsNotTheLastSection) {
    const QString before = QStringLiteral(
        "# ANTS-9999 — Fixture\n"
        "\n"
        "**Status:** draft\n"
        "\n"
        "## Cold-eyes loop log\n"
        "\n"
        "- **Loop 1** — 3 findings, 3 fixed.\n"
        "\n"
        "## 7. Open questions\n"
        "\n"
        "Something that follows the loop log.\n");

    const SpecLog::EditResult r = SpecLog::appendLoop(
        before, QStringLiteral("Loop 2"),
        QStringLiteral("0 findings; converged."), {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    EXPECT_EQ(r.content.count(QStringLiteral("## Cold-eyes loop log")), 1)
        << "ANTS-3651: appending must not create a SECOND loop-log heading";
    EXPECT_TRUE(r.content.contains(QStringLiteral("- **Loop 2** — 0 findings")));

    // Loop 2 must land INSIDE the loop-log section — above the section that
    // follows it — not after the whole document.
    const int loop2  = r.content.indexOf(QStringLiteral("- **Loop 2**"));
    const int nextH2 = r.content.indexOf(QStringLiteral("## 7. Open questions"));
    ASSERT_GE(loop2, 0);
    ASSERT_GE(nextH2, 0);
    EXPECT_LT(loop2, nextH2)
        << "ANTS-3651: the new row belongs in the existing section, not "
           "appended past the section that follows it";
}

// ANTS-3651, the other half — a file that ALREADY carries two loop-log
// headings is ambiguous, and the item asked for a refusal rather than a guess.
// Appending to the first silently deepens a structural defect the author has
// not noticed; refusing names it.
TEST(McpSpecLog, Ants3651RefusesWhenTheLoopLogSectionIsDuplicated) {
    const QString before = QStringLiteral(
        "# ANTS-9999 — Fixture\n"
        "\n"
        "**Status:** draft\n"
        "\n"
        "## Cold-eyes loop log\n"
        "\n"
        "- **Loop 1** — 3 findings.\n"
        "\n"
        "## 7. Open questions\n"
        "\n"
        "Prose.\n"
        "\n"
        "## Cold-eyes loop log\n"
        "\n"
        "- **Loop 7** — stranded under a duplicate heading.\n");

    const SpecLog::EditResult r = SpecLog::appendLoop(
        before, QStringLiteral("Loop 8"), QStringLiteral("body"), {});
    EXPECT_FALSE(r.ok)
        << "ANTS-3651: two loop-log sections is ambiguous — appending to the "
           "first silently deepens the defect";
    EXPECT_EQ(r.code, QStringLiteral("unrecognised_format"));
    EXPECT_TRUE(r.error.contains(QStringLiteral("Cold-eyes loop log")))
        << "the refusal must name what it found: " << r.error.toStdString();
}
