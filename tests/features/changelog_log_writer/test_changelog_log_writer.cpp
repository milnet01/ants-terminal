// ANTS-1548 — feature-conformance test for the changelog_log MCP tool.
// Behavioural on the pure ChangelogLog helpers + the cmdChangelogLog
// handler (m_main-independent, driven against a temp project);
// source-scrape for the contract/descriptor surface.

#include "../../_support/expect.h"
#include "changeloglog.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
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

// ants-v1 roadmap with one bullet carrying a Layman + Kind line.
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

QString clPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("CHANGELOG.md"));
}
QString rmPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "setup-fail: %s\n", path); std::exit(2); }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

// INV-1 — kind → category mapping.
TEST(changelog_log_writer, Inv1KindToCategory) {
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("feature")),
              QStringLiteral("Added"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("implement")),
              QStringLiteral("Added"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("fix")),
              QStringLiteral("Fixed"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("review-fix")),
              QStringLiteral("Fixed"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("security")),
              QStringLiteral("Security"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("refactor")),
              QStringLiteral("Changed"));
    EXPECT_EQ(ChangelogLog::kindToCategory(QStringLiteral("perf")),
              QStringLiteral("Changed"));
}

// INV-2 — bullet formatting.
TEST(changelog_log_writer, Inv2FormatBullet) {
    EXPECT_EQ(ChangelogLog::formatBullet(QStringLiteral("Hi."),
                                         QString(), QString()),
              QStringLiteral("- **Hi.**"));
    EXPECT_EQ(ChangelogLog::formatBullet(QStringLiteral("Hi."),
                                         QString(), QStringLiteral("ANTS-9")),
              QStringLiteral("- **Hi.** (ANTS-9)"));
    EXPECT_EQ(ChangelogLog::formatBullet(QStringLiteral("Hi."),
                                         QStringLiteral("line one"),
                                         QStringLiteral("ANTS-9")),
              QStringLiteral("- **Hi.** (ANTS-9)\n  line one"));
}

// INV-3 — insert at top of an existing category.
TEST(changelog_log_writer, Inv3InsertExistingCategory) {
    const auto r = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kChangelog), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const std::string md = r.markdown.toStdString();
    // New bullet must precede the existing one within Added.
    const auto pNew = md.find("New one.");
    const auto pOld = md.find("Existing added entry.");
    ASSERT_NE(pNew, std::string::npos);
    ASSERT_NE(pOld, std::string::npos);
    EXPECT_LT(pNew, pOld) << "new entry must be most-recent-first";
    EXPECT_FALSE(r.created_category);
}

// INV-4 — create a missing category in canonical order.
TEST(changelog_log_writer, Inv4CreateCategoryInOrder) {
    const auto r = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kChangelog), QStringLiteral("Changed"),
        QStringLiteral("- **A change.**"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.created_category);
    const std::string md = r.markdown.toStdString();
    // Changed (order 1) sits after Added (0) and before Fixed (4).
    const auto pAdded   = md.find("### Added");
    const auto pChanged = md.find("### Changed");
    const auto pFixed   = md.find("### Fixed");
    ASSERT_NE(pChanged, std::string::npos);
    EXPECT_LT(pAdded, pChanged);
    EXPECT_LT(pChanged, pFixed);
}

// INV-5 — refusals on the pure helper.
TEST(changelog_log_writer, Inv5Refusals) {
    auto noUnrel = ChangelogLog::insertUnreleasedEntry(
        QStringLiteral("# Changelog\n\nno unreleased here\n"),
        QStringLiteral("Added"), QStringLiteral("- **x.**"));
    EXPECT_FALSE(noUnrel.ok);
    EXPECT_EQ(noUnrel.code, QStringLiteral("not_unreleased"));

    auto badCat = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kChangelog), QStringLiteral("Frobnicated"),
        QStringLiteral("- **x.**"));
    EXPECT_FALSE(badCat.ok);
    EXPECT_EQ(badCat.code, QStringLiteral("bad_category"));
}

// INV-6 — op:"add" end to end.
TEST(changelog_log_writer, Inv6AddEndToEnd) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add");
    req[QStringLiteral("summary")]    = QStringLiteral("Brand new feature.");
    req[QStringLiteral("kind")]       = QStringLiteral("feature");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-1234");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("category")).toString(),
              QStringLiteral("Added"));
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- **Brand new feature.** (ANTS-1234)"));
    // Under Added, above the existing Added entry.
    EXPECT_LT(md.find("Brand new feature."),
              md.find("Existing added entry."));
}

// INV-7 — op:"add_from_roadmap" reuses headline + Layman.
TEST(changelog_log_writer, Inv7AddFromRoadmap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapBody()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_from_roadmap");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("category")).toString(),
              QStringLiteral("Added"))  // kind: feature
        << "category derived from the bullet's Kind:";
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "- **Did the thing.** (ANTS-0042)"))
        << "headline reused as the summary";
    EXPECT_TRUE(contains(md, "It now does the thing for you."))
        << "Layman line reused as the body";

    // Missing id refuses id_not_in_roadmap.
    QJsonObject bad;
    bad[QStringLiteral("caller_cwd")] = tmp.path();
    bad[QStringLiteral("op")]         = QStringLiteral("add_from_roadmap");
    bad[QStringLiteral("id")]         = QStringLiteral("ANTS-9999");
    const QJsonObject badResp = rc.cmdChangelogLog(bad).object();
    EXPECT_FALSE(badResp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(badResp.value(QStringLiteral("code")).toString(),
              QStringLiteral("id_not_in_roadmap"));
}

// INV-6 refusals — missing fields + no changelog.
TEST(changelog_log_writer, HandlerRefusals) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    RemoteControl rc(nullptr);
    // No caller_cwd.
    {
        QJsonObject req;
        req[QStringLiteral("summary")] = QStringLiteral("x.");
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
    // caller_cwd but no CHANGELOG.md in the dir.
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("summary")]    = QStringLiteral("x.");
        req[QStringLiteral("kind")]       = QStringLiteral("fix");
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("no_changelog"));
    }
    // add with no summary.
    {
        ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("kind")]       = QStringLiteral("fix");
        const auto resp = rc.cmdChangelogLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
}

// ANTS-2040 — a project with a YAML changelog (and no CHANGELOG.md)
// must refuse with format_mismatch (not no_changelog), carrying the
// discovered path + an Edit fallback hint. project_layout already
// discovers data/changelog.yaml, so a bare no_changelog would mislead
// a caller whose reader saw the file.
TEST(changelog_log_writer, Ants2040YamlChangelogFormatMismatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("data")));
    const QString yamlPath =
        QDir(tmp.path()).filePath(QStringLiteral("data/changelog.yaml"));
    ASSERT_TRUE(writeFile(yamlPath,
        QByteArray("- version: 1.0.0\n  date: 2026-01-01\n  body: x\n")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("summary")]    = QStringLiteral("x.");
    req[QStringLiteral("kind")]       = QStringLiteral("fix");
    const auto resp = rc.cmdChangelogLog(req).object();

    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("format_mismatch"))
        << "ANTS-2040: a discovered YAML changelog must refuse with "
           "format_mismatch, not no_changelog";
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("yaml"));
    EXPECT_EQ(resp.value(QStringLiteral("path")).toString(), yamlPath)
        << "ANTS-2040: the refusal names the discovered YAML path";
    EXPECT_FALSE(resp.value(QStringLiteral("hint")).toString().isEmpty())
        << "ANTS-2040: the refusal carries an Edit-fallback hint";
}

// ANTS-2040 — when BOTH a CHANGELOG.md and a YAML changelog exist, the
// Markdown writer still wins (no regression to the format_mismatch
// path for projects that have a real Keep-a-Changelog file).
TEST(changelog_log_writer, Ants2040MarkdownWinsOverYaml) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("data")));
    ASSERT_TRUE(writeFile(
        QDir(tmp.path()).filePath(QStringLiteral("data/changelog.yaml")),
        QByteArray("- version: 1.0.0\n")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("summary")]    = QStringLiteral("Did a thing.");
    req[QStringLiteral("kind")]       = QStringLiteral("fix");
    const auto resp = rc.cmdChangelogLog(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "ANTS-2040: a present CHANGELOG.md must still take the "
           "Markdown write path even when a YAML changelog co-exists";
}

// INV-7 (ANTS-1868) — a wrapped multi-line ROADMAP headline is
// collapsed to a single line before it becomes the bold CHANGELOG
// summary, so the rendered bullet stays a well-formed Markdown list
// item.
TEST(changelog_log_writer, Inv7AddFromRoadmapCollapsesMultiLineHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    // ants-v1 with the headline wrapped across two source lines (the
    // ROADMAP shape that triggered ANTS-1868 in the wild).
    const QByteArray rm =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xE2\x9C\x85 [ANTS-0099] **First line of the headline\n"
        "  that wraps onto the second.**\n"
        "  Body paragraph.\n"
        "  **Layman:** plain words for users.\n"
        "  Kind: feature.\n";
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), rm));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_from_roadmap");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-0099");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const std::string md = readFileStd(clPath(tmp.path()));
    // The bold summary is one continuous line; the wrap+indent has
    // been folded to a single space.
    EXPECT_TRUE(contains(md,
        "- **First line of the headline that wraps onto the "
        "second.** (ANTS-0099)"))
        << "expected one-line summary; got:\n" << md;
    // Defensive: ensure NO hard newline survived inside the bold span.
    const auto bulletPos = md.find("- **First line of the headline");
    ASSERT_NE(bulletPos, std::string::npos);
    const auto closeStar = md.find("**", bulletPos + 4);
    ASSERT_NE(closeStar, std::string::npos);
    const std::string boldSpan = md.substr(bulletPos, closeStar - bulletPos);
    EXPECT_EQ(boldSpan.find('\n'), std::string::npos)
        << "wrapped headline leaked a newline into the bold summary";
}

// INV-8 — contract + descriptor surface (source-scrape).
TEST(changelog_log_writer, Inv8ContractAndDescriptor) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(ci,
        "if (toolName == QStringLiteral(\"changelog_log\"))      return C::Required;"),
        "INV-8: changelog_log classified Required in callerCwdContractFor");
    expect(contains(ci, "t[\"name\"] = \"changelog_log\";"),
        "INV-8: descriptor registered in tools/list");
    expect(contains(ci, "add_from_roadmap"),
        "INV-8: descriptor documents the add_from_roadmap op");
    expect(contains(mw, "registerToolProvider(\"changelog_log\""),
        "INV-8: dispatch registered with explicit contract");
    EXPECT_EQ(0, expect_failures());
}
