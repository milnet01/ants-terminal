// ANTS-3495 — feature-conformance test for changelog_log op:"normalize"
// (reorder the `### <category>` blocks under `## [Unreleased]` into
// canonical Keep-a-Changelog order). ANTS-3381 adds the prose-relocation
// half: a stray flush-left line wedged between blocks folds into the
// bullet above it. Pure-helper INV-1..6 + INV-10..11 exercise
// ChangelogLog::normalizeUnreleased directly; behavioural INV-7..9 +
// INV-12..13 drive RemoteControl::cmdChangelogLog (m_main-independent)
// over a temp project.
// Mirrors the changelog_log_writer / changelog_log_add_batch harnesses.

#include "../../_support/expect.h"
#include "changeloglog.h"
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

// Fixed before Added — the canonical order is Added then Fixed.
const char *kUnordered =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### Fixed\n\n"
    "- **Fix one.** (ANTS-0002)\n\n"
    "### Added\n\n"
    "- **Add one.** (ANTS-0001)\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

const char *kCanonical =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### Added\n\n"
    "- **Add one.** (ANTS-0001)\n\n"
    "### Fixed\n\n"
    "- **Fix one.** (ANTS-0002)\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

// ANTS-3381 — already in canonical category order, but with one stray
// flush-left paragraph after Added's bullet. Line numbers in the comments
// are what `prose_moves` reports.
const char *kStrayProse =
    "# Changelog\n\n"                    // 1, 2
    "## [Unreleased]\n\n"                // 3, 4
    "### Added\n\n"                      // 5, 6
    "- **Add one.** (ANTS-0001)\n\n"     // 7, 8
    "A stray paragraph wedged between category blocks.\n\n"  // 9, 10
    "### Fixed\n\n"                      // 11, 12
    "- **Fix one.** (ANTS-0002)\n\n"     // 13, 14
    "## [0.1.0]\n\n"
    "- old.\n";

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}
// Index of the first occurrence, or npos.
size_t at(const std::string &h, const std::string &n) { return h.find(n); }

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
QStringList orderOf(const QJsonObject &o, const char *key) {
    QStringList out;
    for (const auto v : o.value(QLatin1String(key)).toArray())
        out.append(v.toString());
    return out;
}

}  // namespace

// INV-1 — an out-of-order section is reordered into canonical order,
// bullets travelling under their own heading (non-destructive).
TEST(changelog_log_normalize, Inv1ReorderCanonicalises) {
    const auto r = ChangelogLog::normalizeUnreleased(
        QString::fromUtf8(kUnordered));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.order_before, (QStringList{QStringLiteral("Fixed"),
                                           QStringLiteral("Added")}));
    EXPECT_EQ(r.order_after, (QStringList{QStringLiteral("Added"),
                                          QStringLiteral("Fixed")}));
    const std::string md = r.markdown.toStdString();
    // Added heading now precedes Fixed heading.
    EXPECT_LT(at(md, "### Added"), at(md, "### Fixed"));
    // Each bullet stayed under its own heading.
    EXPECT_LT(at(md, "### Added"), at(md, "- **Add one.** (ANTS-0001)"));
    EXPECT_LT(at(md, "- **Add one.** (ANTS-0001)"), at(md, "### Fixed"));
    EXPECT_LT(at(md, "### Fixed"), at(md, "- **Fix one.** (ANTS-0002)"));
    // Content below the section is untouched.
    EXPECT_TRUE(contains(md, "## [0.1.0] - 2026-01-01"));
    EXPECT_TRUE(contains(md, "- old."));
}

// INV-2 — an already-canonical section is a byte-identical no-op.
TEST(changelog_log_normalize, Inv2AlreadyCanonicalNoOp) {
    const QString in = QString::fromUtf8(kCanonical);
    const auto r = ChangelogLog::normalizeUnreleased(in);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_FALSE(r.changed);
    EXPECT_EQ(r.markdown, in);
}

// INV-3 — a preamble paragraph above the first `### ` is preserved.
TEST(changelog_log_normalize, Inv3PreamblePreserved) {
    const char *withPreamble =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "A note about what's coming.\n\n"
        "### Fixed\n\n"
        "- **Fix one.**\n\n"
        "### Added\n\n"
        "- **Add one.**\n\n"
        "## [0.1.0]\n\n"
        "- old.\n";
    const auto r = ChangelogLog::normalizeUnreleased(
        QString::fromUtf8(withPreamble));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.changed);
    const std::string md = r.markdown.toStdString();
    // Preamble still sits directly under [Unreleased], above both blocks.
    EXPECT_LT(at(md, "## [Unreleased]"), at(md, "A note about what's coming."));
    EXPECT_LT(at(md, "A note about what's coming."), at(md, "### Added"));
    EXPECT_LT(at(md, "### Added"), at(md, "### Fixed"));
}

// INV-4 — a non-canonical heading sorts last; a duplicate category keeps
// its original relative order (stable sort).
TEST(changelog_log_normalize, Inv4UnknownLastDuplicateStable) {
    const char *mixed =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Notes\n\n"
        "- **A custom heading.**\n\n"
        "### Fixed\n\n"
        "- **First fix.**\n\n"
        "### Added\n\n"
        "- **An add.**\n\n"
        "### Fixed\n\n"
        "- **Second fix.**\n\n"
        "## [0.1.0]\n\n"
        "- old.\n";
    const auto r = ChangelogLog::normalizeUnreleased(QString::fromUtf8(mixed));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.changed);
    // Added, then the two Fixed blocks in original order, then unknown last.
    EXPECT_EQ(r.order_after, (QStringList{
                                 QStringLiteral("Added"),
                                 QStringLiteral("Fixed"),
                                 QStringLiteral("Fixed"),
                                 QStringLiteral("Notes")}));
    const std::string md = r.markdown.toStdString();
    EXPECT_LT(at(md, "- **First fix.**"), at(md, "- **Second fix.**"));
    EXPECT_LT(at(md, "### Added"), at(md, "### Notes"));
    EXPECT_LT(at(md, "- **Second fix.**"), at(md, "### Notes"));
}

// INV-5 — refusals: not_unreleased and feature_grouped_section.
TEST(changelog_log_normalize, Inv5Refusals) {
    const auto noUnrel = ChangelogLog::normalizeUnreleased(
        QString::fromUtf8("# Changelog\n\n## [0.1.0]\n\n- x.\n"));
    EXPECT_FALSE(noUnrel.ok);
    EXPECT_EQ(noUnrel.code, QStringLiteral("not_unreleased"));

    const char *featureGrouped =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### ANTS-0001 — Live search (2026-07-01)\n\n"
        "**Added**\n\n"
        "- **A feature.**\n\n"
        "## [0.1.0]\n\n"
        "- old.\n";
    const auto fg = ChangelogLog::normalizeUnreleased(
        QString::fromUtf8(featureGrouped));
    EXPECT_FALSE(fg.ok);
    EXPECT_EQ(fg.code, QStringLiteral("feature_grouped_section"));
}

// INV-6 — prose a fold cannot reach is left in place and surfaced. A
// heading between the prose and the nearest bullet is a barrier: indenting
// the line would not put it under that bullet, so ANTS-3381 declines the
// fold and the ANTS-2125 advisory still fires.
TEST(changelog_log_normalize, Inv6UnfoldableProseAdvisorySurfaced) {
    const char *behindHeading =
        "# Changelog\n\n"          // 1, 2
        "## [Unreleased]\n\n"      // 3, 4
        "### Added\n\n"            // 5, 6
        "- **Add one.**\n\n"       // 7, 8
        "#### Notes\n\n"           // 9, 10
        "A paragraph under a sub-heading.\n\n"   // 11, 12
        "## [0.1.0]\n\n"
        "- old.\n";
    const QString in = QString::fromUtf8(behindHeading);
    const auto r = ChangelogLog::normalizeUnreleased(in);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.prose_moves.isEmpty());
    EXPECT_FALSE(r.changed);
    EXPECT_EQ(r.markdown, in);
    EXPECT_TRUE(r.malformed_section);
    EXPECT_EQ(r.malformed_line, 11);
}

// INV-10 — a stray flush-left line after a bullet is folded into that
// bullet as a two-space continuation, in place; the fold is reported and
// the advisory clears.
TEST(changelog_log_normalize, Inv10StrayProseFoldedUnderBullet) {
    const QString in = QString::fromUtf8(kStrayProse);
    const auto r = ChangelogLog::normalizeUnreleased(in);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.prose_moves.size(), 1);
    EXPECT_EQ(r.prose_moves.at(0).from_line, 9);
    EXPECT_EQ(r.prose_moves.at(0).under_line, 7);
    EXPECT_EQ(r.prose_moves.at(0).text,
              QStringLiteral("A stray paragraph wedged between category "
                             "blocks."));
    // The section was ALREADY in canonical order — prose alone is a change.
    EXPECT_EQ(r.order_before, r.order_after);
    EXPECT_TRUE(r.changed);
    // Folded in place: two-space continuation, same line count, and the
    // scanner no longer sees it.
    const std::string md = r.markdown.toStdString();
    EXPECT_TRUE(contains(md,
        "- **Add one.** (ANTS-0001)\n\n  A stray paragraph wedged"));
    EXPECT_EQ(r.markdown.count(QLatin1Char('\n')),
              in.count(QLatin1Char('\n')));
    EXPECT_FALSE(r.malformed_section);
}

// INV-11 — every stray line folds under its OWN nearest preceding bullet,
// reported in file order, and the folds survive a reorder in the same call.
TEST(changelog_log_normalize, Inv11MultipleStraysEachUnderItsOwnBullet) {
    const char *twoStrays =
        "# Changelog\n\n"          // 1, 2
        "## [Unreleased]\n\n"      // 3, 4
        "### Fixed\n\n"            // 5, 6
        "- **Fix one.**\n"         // 7
        "Stray under fix.\n\n"     // 8, 9
        "### Added\n\n"            // 10, 11
        "- **Add one.**\n\n"       // 12, 13
        "---\n\n"                  // 14, 15
        "## [0.1.0]\n\n"
        "- old.\n";
    const auto r = ChangelogLog::normalizeUnreleased(
        QString::fromUtf8(twoStrays));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.prose_moves.size(), 2);
    EXPECT_EQ(r.prose_moves.at(0).from_line, 8);
    EXPECT_EQ(r.prose_moves.at(0).under_line, 7);
    EXPECT_EQ(r.prose_moves.at(1).from_line, 14);
    EXPECT_EQ(r.prose_moves.at(1).under_line, 12);
    // A bare `---` is a thematic break, not a bullet — it folds like prose.
    EXPECT_EQ(r.prose_moves.at(1).text, QStringLiteral("---"));
    EXPECT_TRUE(r.changed);
    const std::string md = r.markdown.toStdString();
    EXPECT_TRUE(contains(md, "- **Fix one.**\n  Stray under fix."));
    EXPECT_TRUE(contains(md, "- **Add one.**\n\n  ---"));
    // Each fold travelled with its own block through the reorder.
    EXPECT_LT(at(md, "### Added"), at(md, "### Fixed"));
    EXPECT_LT(at(md, "  ---"), at(md, "### Fixed"));
    EXPECT_FALSE(r.malformed_section);
}

// INV-7 — handler write path: rewrites CHANGELOG.md with the reordered body.
TEST(changelog_log_normalize, Inv7HandlerWrites) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kUnordered)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("normalize");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("changed")).toBool());
    // ANTS-3723 — bytes_written is now the ADDED-bytes delta (matching
    // roadmap_log), and a reorder adds nothing: the byte count is identical,
    // only the order changed. That is a stronger claim than the old
    // whole-file `> 0`, which any successful write satisfied. `file_bytes`
    // carries the size the field used to report.
    EXPECT_EQ(resp.value(QStringLiteral("bytes_written")).toInt(), 0);
    EXPECT_GT(resp.value(QStringLiteral("file_bytes")).toInt(), 0);
    EXPECT_EQ(orderOf(resp, "order_after"),
              (QStringList{QStringLiteral("Added"), QStringLiteral("Fixed")}));
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_LT(at(md, "### Added"), at(md, "### Fixed"));
}

// INV-8 — handler dry_run: nothing is written.
TEST(changelog_log_normalize, Inv8HandlerDryRunWritesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kUnordered)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("normalize");
    req[QStringLiteral("dry_run")]    = true;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("changed")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
    // File is untouched — still Fixed-before-Added on disk.
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_LT(at(md, "### Fixed"), at(md, "### Added"));
}

// INV-9 — handler no-op: an already-canonical file is not rewritten.
TEST(changelog_log_normalize, Inv9HandlerAlreadyCanonicalNoWrite) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kCanonical)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("normalize");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("changed")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
    // Content unchanged.
    EXPECT_EQ(readFileStd(clPath(tmp.path())), std::string(kCanonical));
}

// INV-12 — handler write path with stray prose: the fold is written, and
// reported as `prose_moved` + `moves[]`. The section was already in
// canonical order, so this also proves prose alone is enough to write.
TEST(changelog_log_normalize, Inv12HandlerWritesProseFold) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kStrayProse)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("normalize");
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("prose_moved")).toInt(), 1);
    const QJsonArray moves =
        resp.value(QStringLiteral("moves")).toArray();
    ASSERT_EQ(moves.size(), 1);
    EXPECT_EQ(moves.at(0).toObject().value(QStringLiteral("from_line")).toInt(),
              9);
    EXPECT_EQ(moves.at(0).toObject().value(QStringLiteral("under_line")).toInt(),
              7);
    // A fold adds exactly its two indent spaces — unlike a pure reorder,
    // which reports a 0 delta (INV-7).
    EXPECT_EQ(resp.value(QStringLiteral("bytes_written")).toInt(), 2);
    // No advisory: the only stray line was foldable.
    EXPECT_FALSE(resp.contains(QStringLiteral("advisory")));
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md,
        "- **Add one.** (ANTS-0001)\n\n  A stray paragraph wedged"));
}

// INV-13 — handler dry_run with stray prose: nothing is written, but
// `moves[]` previews every fold. This preview is the only guard against
// the failure mode the accepted policy tolerates (a stand-alone paragraph
// being absorbed by the entry above it), so it must not be write-only.
TEST(changelog_log_normalize, Inv13HandlerDryRunPreviewsProseFold) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kStrayProse)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("normalize");
    req[QStringLiteral("dry_run")]    = true;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("prose_moved")).toInt(), 1);
    ASSERT_EQ(resp.value(QStringLiteral("moves")).toArray().size(), 1);
    EXPECT_EQ(resp.value(QStringLiteral("moves")).toArray().at(0).toObject()
                  .value(QStringLiteral("text")).toString(),
              QStringLiteral("A stray paragraph wedged between category "
                             "blocks."));
    EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));
    // File untouched — the paragraph is still flush-left on disk.
    EXPECT_EQ(readFileStd(clPath(tmp.path())), std::string(kStrayProse));
}
