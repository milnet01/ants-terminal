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

// INV-4 — input order preserved: equals two sequential single-op adds.
TEST(changelog_log_add_batch, Inv4InputOrderSequentialEquivalent) {
    // Batch path.
    QTemporaryDir batchDir;
    ASSERT_TRUE(batchDir.isValid());
    ASSERT_TRUE(writeFile(clPath(batchDir.path()), QByteArray(kChangelog)));
    {
        QJsonArray entries;
        entries.append(addEntry(QStringLiteral("Entry zero."),
                                QStringLiteral("feature")));
        entries.append(addEntry(QStringLiteral("Entry one."),
                                QStringLiteral("feature")));
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = batchDir.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_batch");
        req[QStringLiteral("entries")]    = entries;
        ASSERT_TRUE(rc.cmdChangelogLog(req).object()
                        .value(QStringLiteral("ok")).toBool());
    }
    // Sequential path.
    QTemporaryDir seqDir;
    ASSERT_TRUE(seqDir.isValid());
    ASSERT_TRUE(writeFile(clPath(seqDir.path()), QByteArray(kChangelog)));
    {
        RemoteControl rc(nullptr);
        for (const char *s : {"Entry zero.", "Entry one."}) {
            QJsonObject req;
            req[QStringLiteral("caller_cwd")] = seqDir.path();
            req[QStringLiteral("op")]         = QStringLiteral("add");
            req[QStringLiteral("summary")]    = QString::fromUtf8(s);
            req[QStringLiteral("kind")]       = QStringLiteral("feature");
            ASSERT_TRUE(rc.cmdChangelogLog(req).object()
                            .value(QStringLiteral("ok")).toBool());
        }
    }
    EXPECT_EQ(readFileStd(clPath(batchDir.path())),
              readFileStd(clPath(seqDir.path())))
        << "add_batch must be byte-identical to N sequential op:add calls";
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
