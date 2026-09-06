// ANTS-2044 — feature-conformance test for changelog_log op:"add_batch".
// Behavioural against cmdChangelogLog (m_main-independent), driven over a
// temp project. Mirrors the changelog_log_writer harness.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

const char *kChangelog =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### Added\n\n"
    "- **Existing added entry.** (ANTS-0001)\n\n"
    "### Fixed\n\n"
    "- **Existing fix.** (ANTS-0002)\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

QByteArray roadmapBody() {
    return QByteArray(
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xE2\x9C\x85 [ANTS-0042] **Did the thing.**\n"
        "  Body paragraph with detail.\n"
        "  **Layman:** It now does the thing for you.\n"
        "  Kind: feature.\n"
        "  Source: test.\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}
std::string readFileStd(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}
bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}
QString clPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("CHANGELOG.md"));
}
QString rmPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}
QJsonObject addEntry(const QString &summary, const QString &kind,
                     const QString &id = QString()) {
    QJsonObject e;
    e[QStringLiteral("summary")] = summary;
    e[QStringLiteral("kind")]    = kind;
    if (!id.isEmpty()) e[QStringLiteral("id")] = id;
    return e;
}

}  // namespace

// INV-1 — a clean batch applies every entry under one commit.
TEST(changelog_log_add_batch, Inv1CleanBatchAllApply) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));

    QJsonArray entries;
    entries.append(addEntry(QStringLiteral("First feature."),
                            QStringLiteral("feature"),
                            QStringLiteral("ANTS-1001")));
    entries.append(addEntry(QStringLiteral("A bug fix."),
                            QStringLiteral("fix"),
                            QStringLiteral("ANTS-1002")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_batch");
    req[QStringLiteral("entries")]    = entries;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 2);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 0);
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- **First feature.** (ANTS-1001)"));
    EXPECT_TRUE(contains(md, "- **A bug fix.** (ANTS-1002)"));
}

// INV-2 — mixed modes (add + add_from_roadmap) in one batch.
TEST(changelog_log_add_batch, Inv2MixedModes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapBody()));

    QJsonArray entries;
    entries.append(addEntry(QStringLiteral("Hand-written one."),
                            QStringLiteral("fix")));
    QJsonObject fromRm;  // id-only → add_from_roadmap
    fromRm[QStringLiteral("id")] = QStringLiteral("ANTS-0042");
    entries.append(fromRm);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_batch");
    req[QStringLiteral("entries")]    = entries;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 2);
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- **Hand-written one.**"));
    EXPECT_TRUE(contains(md, "- **Did the thing.** (ANTS-0042)"))
        << "headline pulled from ROADMAP";
    EXPECT_TRUE(contains(md, "It now does the thing for you."))
        << "Layman line pulled as the body";
}

// INV-3 — a bad entry is skipped, the rest apply.
TEST(changelog_log_add_batch, Inv3BadEntrySkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));

    QJsonArray entries;
    entries.append(addEntry(QStringLiteral("Good one."),
                            QStringLiteral("feature")));
    // Bad: explicit invalid category.
    QJsonObject bad;
    bad[QStringLiteral("summary")]  = QStringLiteral("Bad cat.");
    bad[QStringLiteral("category")] = QStringLiteral("Frobnicated");
    entries.append(bad);
    // Bad: neither summary nor id.
    QJsonObject empty;
    empty[QStringLiteral("kind")] = QStringLiteral("fix");
    entries.append(empty);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_batch");
    req[QStringLiteral("entries")]    = entries;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 1);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 2);
    const QJsonArray skipped = resp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 2);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("index")).toInt(),
              1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_category"));
    EXPECT_EQ(skipped.at(1).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- **Good one.**"));
    EXPECT_FALSE(contains(md, "Bad cat."));
}

// INV-4 (ANTS-4854) — the RESULT reads in input order.
//
// Each insert prepends to the top of its category, so applying [E0, E1] in
// input order put E1 above E0: the batch came out backwards from the array
// that was written. The old contract here was byte-identity with N sequential
// op:add calls, which that behaviour satisfied — and which no caller checks,
// while every caller reads the file. Entries are now applied in REVERSE, so
// the file reads E0 then E1.
//
// Sequential-equivalence is deliberately withdrawn as an ordering claim: a
// batch is one act with an order the caller wrote down, where N calls are N
// acts. Same entries, same categories, same count either way.
TEST(changelog_log_add_batch, Inv4ResultReadsInInputOrder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(clPath(dir.path()), QByteArray(kChangelog)));

    QJsonArray entries;
    entries.append(addEntry(QStringLiteral("Entry zero."),
                            QStringLiteral("feature")));
    entries.append(addEntry(QStringLiteral("Entry one."),
                            QStringLiteral("feature")));
    entries.append(addEntry(QStringLiteral("Entry two."),
                            QStringLiteral("feature")));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = dir.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_batch");
    req[QStringLiteral("entries")]    = entries;
    const QJsonObject env = rc.cmdChangelogLog(req).object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    const std::string body = readFileStd(clPath(dir.path()));
    const auto p0 = body.find("Entry zero.");
    const auto p1 = body.find("Entry one.");
    const auto p2 = body.find("Entry two.");
    ASSERT_NE(p0, std::string::npos);
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    EXPECT_LT(p0, p1) << "entry 0 must read above entry 1";
    EXPECT_LT(p1, p2) << "entry 1 must read above entry 2";

    // applied[] still reports in INPUT order, whatever order the writes ran
    // in — a caller diffing against its own array indexes by position.
    const QJsonArray applied = env.value(QStringLiteral("applied")).toArray();
    ASSERT_EQ(applied.size(), 3);
    for (int i = 0; i < applied.size(); ++i)
        EXPECT_EQ(applied.at(i).toObject().value(QStringLiteral("index")).toInt(),
                  i)
            << "applied[] must stay in input order";
}

// INV-5 — empty / missing entries refuses bad_args (no write).
TEST(changelog_log_add_batch, Inv5EmptyEntriesBadArgs) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    const std::string before = readFileStd(clPath(tmp.path()));

    RemoteControl rc(nullptr);
    // Empty array.
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        req[QStringLiteral("entries")]    = QJsonArray();
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
    }
    // Missing entries entirely.
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
    }
    EXPECT_EQ(readFileStd(clPath(tmp.path())), before)
        << "a bad_args refusal must not touch the file";
}

// INV-6 — top-level refusals match the single op.
TEST(changelog_log_add_batch, Inv6TopLevelRefusals) {
    RemoteControl rc(nullptr);
    // Missing caller_cwd.
    {
        QJsonObject req;
        req[QStringLiteral("op")]      = QStringLiteral("add_batch");
        req[QStringLiteral("entries")] = QJsonArray{addEntry(
            QStringLiteral("x."), QStringLiteral("fix"))};
        EXPECT_EQ(rc.cmdChangelogLog(req).object()
                      .value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
    // No changelog of any kind.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        req[QStringLiteral("entries")]    = QJsonArray{addEntry(
            QStringLiteral("x."), QStringLiteral("fix"))};
        EXPECT_EQ(rc.cmdChangelogLog(req).object()
                      .value(QStringLiteral("code")).toString(),
                  QStringLiteral("no_changelog"));
    }
    // YAML changelog → format_mismatch (ANTS-2040 parity).
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("data")));
        ASSERT_TRUE(writeFile(
            QDir(tmp.path()).filePath(QStringLiteral("data/changelog.yaml")),
            QByteArray("- version: 1.0.0\n")));
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        req[QStringLiteral("entries")]    = QJsonArray{addEntry(
            QStringLiteral("x."), QStringLiteral("fix"))};
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("format_mismatch"));
        EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
                  QStringLiteral("yaml"));
    }
}

// ANTS-2136 — dry_run previews the insert without touching CHANGELOG.md,
// for both the single op and add_batch. The file must be byte-identical
// to the pre-call content after a dry_run.
TEST(changelog_log_add_batch, Inv7DryRunWritesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    const std::string before = readFileStd(clPath(tmp.path()));

    RemoteControl rc(nullptr);

    // Single op:add dry_run — preview fields present, no write.
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add");
        req[QStringLiteral("summary")]    = QStringLiteral("Previewed only.");
        req[QStringLiteral("kind")]       = QStringLiteral("fix");
        req[QStringLiteral("id")]         = QStringLiteral("ANTS-9001");
        req[QStringLiteral("dry_run")]    = true;
        const auto resp = rc.cmdChangelogLog(req).object();

        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
        EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("category")).toString(),
                  QStringLiteral("Fixed"));
        EXPECT_TRUE(resp.contains(QStringLiteral("bytes")));
        EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
        EXPECT_TRUE(contains(
            resp.value(QStringLiteral("bullet")).toString().toStdString(),
            "- **Previewed only.** (ANTS-9001)"));
        EXPECT_EQ(readFileStd(clPath(tmp.path())), before)
            << "single-op dry_run must not write CHANGELOG.md";
    }

    // add_batch dry_run — applied[] echoed, no write.
    {
        QJsonArray entries;
        entries.append(addEntry(QStringLiteral("Batch preview A."),
                                QStringLiteral("feature"),
                                QStringLiteral("ANTS-9002")));
        entries.append(addEntry(QStringLiteral("Batch preview B."),
                                QStringLiteral("fix"),
                                QStringLiteral("ANTS-9003")));
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        req[QStringLiteral("entries")]    = entries;
        req[QStringLiteral("dry_run")]    = true;
        const auto resp = rc.cmdChangelogLog(req).object();

        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
        EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 2);
        EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 0);
        EXPECT_TRUE(resp.contains(QStringLiteral("bytes")));
        EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
        EXPECT_EQ(readFileStd(clPath(tmp.path())), before)
            << "add_batch dry_run must not write CHANGELOG.md";
    }
}

// ANTS-4395 — each applied entry reports the line it ACTUALLY occupies.
//
// A two-entry batch returned applied:[{index:0, line:26}, {index:1,
// line:26}]. Two entries appended under one category cannot both occupy line
// 26: insertion is at the category head, so the second takes the first's line
// and pushes it down. `res.line` is correct at insert time and goes stale the
// moment a later entry lands in the same category — so only the first could
// ever be right, and the single-entry op:"add" path was always correct, which
// is why it went unnoticed.
//
// The file was correct throughout; this is the envelope. But a caller using
// the returned line to cite or re-read the entry it just wrote read the wrong
// one for every entry after the first.
TEST(changelog_log_add_batch, Ants4395PerEntryLinesAreDistinctAndReal) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));

    QJsonArray entries;
    // BOTH in the same category — the shape that collides.
    entries.append(addEntry(QStringLiteral("First added thing."),
                            QStringLiteral("feature"),
                            QStringLiteral("ANTS-1010")));
    entries.append(addEntry(QStringLiteral("Second added thing."),
                            QStringLiteral("feature"),
                            QStringLiteral("ANTS-1016")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_batch");
    req[QStringLiteral("entries")]    = entries;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QJsonArray applied = resp.value(QStringLiteral("applied")).toArray();
    ASSERT_EQ(applied.size(), 2);
    const int l0 = applied.at(0).toObject().value(QStringLiteral("line")).toInt();
    const int l1 = applied.at(1).toObject().value(QStringLiteral("line")).toInt();
    EXPECT_NE(l0, l1)
        << "two entries in one category cannot occupy the same line";

    // And each reported line must actually hold that entry in the file — the
    // point is not that the numbers differ but that they are TRUE.
    const QStringList lines =
        QString::fromStdString(readFileStd(clPath(tmp.path())))
            .split(QLatin1Char('\n'));
    ASSERT_GT(lines.size(), qMax(l0, l1));
    EXPECT_TRUE(lines.at(l0 - 1).contains(QStringLiteral("(ANTS-1010)")))
        << "line " << l0 << " is: " << lines.at(l0 - 1).toStdString();
    EXPECT_TRUE(lines.at(l1 - 1).contains(QStringLiteral("(ANTS-1016)")))
        << "line " << l1 << " is: " << lines.at(l1 - 1).toStdString();

    // The scaffolding field must not leak into the envelope.
    EXPECT_FALSE(applied.at(0).toObject().contains(QStringLiteral("_bullet_head")));
}
