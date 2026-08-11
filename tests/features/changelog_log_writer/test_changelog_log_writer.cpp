// ANTS-1548 — feature-conformance test for the changelog_log MCP tool.
// Behavioural on the pure ChangelogLog helpers + the cmdChangelogLog
// handler (m_main-independent, driven against a temp project);
// source-scrape for the contract/descriptor surface.

#include "../../_support/expect.h"
#include "changeloglog.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
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

// ANTS-2125 — a CHANGELOG whose `## [Unreleased]` section interleaves
// non-heading prose (a `---` rule + a flush-left paragraph) between the
// `### Added` block and the later `### Fixed` heading — the stray-footer
// shape DOOM Ants hit. The `---` sits on line 9 (1-based).
const char *kMalformedChangelog =
    "# Changelog\n\n"            // 1, 2
    "## [Unreleased]\n\n"        // 3, 4
    "### Added\n\n"              // 5, 6
    "- **Existing added entry.** (ANTS-0001)\n\n"  // 7, 8
    "---\n"                      // 9  <- first interleaved prose line
    "This project is licensed under the GPL.\n\n"  // 10, 11
    "### Fixed\n\n"             // 12, 13
    "- **Existing fix.** (ANTS-0002)\n\n"          // 14, 15
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

// ANTS-3416 — a FEATURE-GROUPED `## [Unreleased]`: its direct `### `
// children are dated topic headings (MAME Curator house style) with
// `**Bold**` category runs (`**Fixed**` / inline `**Security:**`) beneath
// — NOT flat Keep-a-Changelog category words. `### ` is not the category
// slot here, so a flat-category insert would mis-order; the writer refuses.
// The first dated topic heading sits on line 5 (1-based). The `\xE2\x80\x94`
// is an em-dash.
const char *kFeatureGroupedChangelog =
    "# Changelog\n\n"                                          // 1, 2
    "## [Unreleased]\n\n"                                      // 3, 4
    "### MAME-0042 \xE2\x80\x94 curator dedup (2026-07-02)\n\n" // 5, 6
    "Intro paragraph about the topic.\n\n"                     // 7, 8
    "**Fixed**\n\n"                                            // 9, 10
    "- Something got fixed.\n\n"                               // 11, 12
    "**Security:** a note.\n\n"                                // 13, 14
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

// ANTS-2125 — the pure helper flags a malformed Unreleased section
// (non-heading prose between `### ` category blocks) without altering
// the insert, and leaves a clean section unflagged.
TEST(changelog_log_writer, Ants2125MalformedSectionAdvisoryHelper) {
    const auto bad = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kMalformedChangelog), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(bad.ok) << bad.error.toStdString();
    EXPECT_TRUE(bad.malformed_section)
        << "interleaved `---`/footer prose must be flagged";
    EXPECT_EQ(bad.malformed_line, 9)
        << "first offending line is the `---` rule";
    // The insert still landed (non-blocking advisory).
    EXPECT_TRUE(contains(bad.markdown.toStdString(), "New one."));

    const auto clean = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kChangelog), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(clean.ok) << clean.error.toStdString();
    EXPECT_FALSE(clean.malformed_section)
        << "a well-formed section must not be flagged";
    EXPECT_EQ(clean.malformed_line, -1);
}

// ANTS-4103 — two shapes the advisory used to call malformed and should not.
// Both arrived attached to a SUCCESSFUL write, so they read as "the write
// worked but your file is broken" and invited a restructuring edit to a file
// that needed none.
TEST(changelog_log_writer, Ants4103AdvisoryFalsePositives) {
    // (a) A trailing HTML comment. It does not render, so it is not prose a
    // reader ever sees wedged between blocks.
    const char *withComment =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **Existing added entry.** (ANTS-0001)\n\n"
        "<!-- maintained by changelog_log;\n"
        "     do not hand-edit above this line -->\n\n"
        "## [0.1.0] - 2026-01-01\n\n"
        "- old.\n";
    const auto cmt = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(withComment), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(cmt.ok) << cmt.error.toStdString();
    EXPECT_FALSE(cmt.malformed_section)
        << "a multi-line HTML comment is not interleaved prose";

    // (b) Prose between a `### ` heading and that block's FIRST bullet — the
    // heading's own description. This is the general form of the shape the
    // verb's own op:add_subsection writes (dated topic heading, blank,
    // flush-left body prose, blank, then bullets); that exact layout reaches
    // the scanner through op:normalize, since a purely feature-grouped
    // section refuses a flat add earlier (ANTS-3416, asserted separately).
    const char *withIntro =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "Three defects reported by other sessions, all in one verb.\n\n"
        "- **First fix.** (ANTS-1)\n\n"
        "## [0.1.0] - 2026-01-01\n\n"
        "- old.\n";
    const auto intro = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(withIntro), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(intro.ok) << intro.error.toStdString();
    EXPECT_FALSE(intro.malformed_section)
        << "the scanner must not flag add_subsection's own canonical output";

    // …and the stray-footer shape ANTS-2125 exists for still trips, because
    // it arrives AFTER the block's bullets. Same fixture, one line moved.
    const char *strayAfterBullet =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **First fix.** (ANTS-1)\n\n"
        "This project is licensed under the GPL.\n\n"
        "## [0.1.0] - 2026-01-01\n\n"
        "- old.\n";
    const auto stray = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(strayAfterBullet), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    ASSERT_TRUE(stray.ok) << stray.error.toStdString();
    EXPECT_TRUE(stray.malformed_section)
        << "prose after a block's bullets is still the stray-footer shape";
    EXPECT_EQ(stray.malformed_line, 9);
}

// ANTS-2125 — the handler surfaces the advisory string on a successful
// write into a malformed section, and omits it for a clean one.
TEST(changelog_log_writer, Ants2125MalformedSectionAdvisoryHandler) {
    auto runAdd = [](const char *changelog) -> QJsonObject {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        writeFile(clPath(tmp.path()), QByteArray(changelog));
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add");
        req[QStringLiteral("summary")]    = QStringLiteral("Fresh entry.");
        req[QStringLiteral("kind")]       = QStringLiteral("feature");
        return rc.cmdChangelogLog(req).object();
    };

    const QJsonObject badResp = runAdd(kMalformedChangelog);
    ASSERT_TRUE(badResp.value(QStringLiteral("ok")).toBool())
        << "advisory is non-blocking — the write still succeeds";
    EXPECT_FALSE(badResp.value(QStringLiteral("advisory")).toString().isEmpty())
        << "a malformed Unreleased section must surface an advisory";

    const QJsonObject cleanResp = runAdd(kChangelog);
    ASSERT_TRUE(cleanResp.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(cleanResp.contains(QStringLiteral("advisory")))
        << "a clean section must not carry an advisory";
}

// ANTS-3416 — a feature-grouped `## [Unreleased]` (dated `### ` topics +
// `**Bold**` category runs) is REFUSED at the pure helper with
// `feature_grouped_section`, while a flat category layout (clean OR the
// ANTS-2125 messy shape, both carrying canonical `### ` headings) is not.
TEST(changelog_log_writer, Ants3416FeatureGroupedRefusalHelper) {
    const auto grouped = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kFeatureGroupedChangelog), QStringLiteral("Fixed"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    EXPECT_FALSE(grouped.ok) << "a feature-grouped section must be refused";
    EXPECT_EQ(grouped.code, QStringLiteral("feature_grouped_section"));
    EXPECT_TRUE(contains(grouped.error.toStdString(), "line 5"))
        << "the refusal names the first dated-topic heading line";
    EXPECT_TRUE(grouped.markdown.isEmpty())
        << "a refusal must not produce a rewritten body";

    const auto flat = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kChangelog), QStringLiteral("Fixed"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    EXPECT_TRUE(flat.ok) << flat.error.toStdString();

    // A flat-but-messy section (canonical `### ` headings + stray prose)
    // stays the ANTS-2125 non-blocking advisory, NOT a refusal.
    const auto malformed = ChangelogLog::insertUnreleasedEntry(
        QString::fromUtf8(kMalformedChangelog), QStringLiteral("Added"),
        QStringLiteral("- **New one.** (ANTS-9)"));
    EXPECT_TRUE(malformed.ok)
        << "a flat category layout must never be mistaken for feature-grouped";
    EXPECT_TRUE(malformed.malformed_section);
}

// ANTS-3416 — the handler propagates the refusal and leaves CHANGELOG.md
// byte-for-byte untouched (the write is skipped on !res.ok).
TEST(changelog_log_writer, Ants3416FeatureGroupedRefusalHandler) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()),
                          QByteArray(kFeatureGroupedChangelog)));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add");
    req[QStringLiteral("summary")]    = QStringLiteral("Fresh entry.");
    req[QStringLiteral("kind")]       = QStringLiteral("fix");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("feature_grouped_section"));
    EXPECT_EQ(readFileStd(clPath(tmp.path())),
              std::string(kFeatureGroupedChangelog))
        << "a refused write must not touch the file";
}

// ANTS-2127 — op:add_from_roadmap must reuse the UNTRUNCATED headline
// (BulletRecord.headlineFull), not the 120-char display cap, so a long
// roadmap headline does not leak a `…` ellipsis into the rendered
// CHANGELOG bold summary.
TEST(changelog_log_writer, Ants2127LongHeadlineNotTruncated) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    // A headline well over the 120-char display cap (ANTS-1811).
    const QByteArray rm =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xE2\x9C\x85 [ANTS-0150] **changelog_log surfaces a malformed "
        "section advisory when the active Unreleased section interleaves "
        "non-heading prose between its category blocks and would compound "
        "it.**\n"
        "  **Layman:** plain words for users.\n"
        "  Kind: enhancement.\n";
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), rm));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_from_roadmap");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-0150");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const std::string md = readFileStd(clPath(tmp.path()));
    // The full sentence (its tail) survives; no ellipsis leaked.
    EXPECT_TRUE(contains(md, "would compound it.** (ANTS-0150)"))
        << "untruncated headline must render in full; got:\n" << md;
    EXPECT_FALSE(contains(md, "\xE2\x80\xA6"))  // U+2026 horizontal ellipsis
        << "the 120-char display cap leaked a `…` into the CHANGELOG";
}

// ANTS-3723 — `bytes_written` is the ADDED-bytes delta, not the whole file,
// and `file_bytes` carries the size. Same convention as roadmap_log
// (ANTS-3702): identical field names must not mean opposite things across
// two sibling verbs, because the reason a caller trusts either number is
// that the pair agree.
TEST(changelog_log_writer, Ants3723BytesWrittenIsDeltaNotWholeFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kChangelog)));
    const qint64 before = QFileInfo(clPath(tmp.path())).size();

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add");
    req[QStringLiteral("category")]   = QStringLiteral("Fixed");
    req[QStringLiteral("summary")]    = QStringLiteral("A short new entry.");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const qint64 after = QFileInfo(clPath(tmp.path())).size();
    const qint64 written =
        static_cast<qint64>(resp.value(QStringLiteral("bytes_written")).toDouble());
    const qint64 fileBytes =
        static_cast<qint64>(resp.value(QStringLiteral("file_bytes")).toDouble());
    EXPECT_EQ(written, after - before)
        << "bytes_written must be what this op ADDED";
    EXPECT_EQ(fileBytes, after);
    EXPECT_LT(written, before)
        << "a short entry reported as the whole file is the ANTS-3723 defect";
}

// INV-8 — contract + descriptor surface (source-scrape).
TEST(changelog_log_writer, Inv8ContractAndDescriptor) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
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
