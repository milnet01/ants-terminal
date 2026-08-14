// ANTS-3584 — feature-conformance test for changelog_log op:"add_subsection"
// (insert a dated `### <date> <Category> — <headline>` block at the top of
// `## [Unreleased]`). Pure-helper INV-1..6 exercise
// ChangelogLog::insertUnreleasedSubsection directly; behavioural INV-7..9
// drive RemoteControl::cmdChangelogLog (m_main-independent) over a temp
// project. Mirrors the changelog_log_normalize harness.

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

const char *kFlat =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### 2026-07-20 Added — Existing dated topic (PROJ-1)\n\n"
    "An earlier entry.\n\n"
    "- **An earlier bullet.**\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}
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
QJsonObject bullet(const QString &summary, const QString &body,
                   const QString &id) {
    QJsonObject o;
    o[QStringLiteral("summary")] = summary;
    if (!body.isEmpty()) o[QStringLiteral("body")] = body;
    if (!id.isEmpty())   o[QStringLiteral("id")]   = id;
    return o;
}

}  // namespace

// INV-1 — a dated subsection is inserted at the top with heading, prose, bullets.
TEST(changelog_log_subsection, Inv1InsertsDatedBlock) {
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(kFlat), QStringLiteral("2026-07-24"),
        QStringLiteral("Fixed"), QStringLiteral("New thing works now (PROJ-9)"),
        QStringLiteral("A layman explanation of the fix."),
        QStringList{ChangelogLog::formatBullet(
            QStringLiteral("The change."), QStringLiteral("detail."),
            QStringLiteral("PROJ-9"))});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const std::string md = r.markdown.toStdString();
    EXPECT_TRUE(contains(
        md, "### 2026-07-24 Fixed — New thing works now (PROJ-9)"));
    EXPECT_TRUE(contains(md, "A layman explanation of the fix."));
    EXPECT_TRUE(contains(md, "- **The change.** (PROJ-9)"));
    // Heading sits below [Unreleased] and above the prose + bullet.
    EXPECT_LT(at(md, "## [Unreleased]"),
              at(md, "### 2026-07-24 Fixed"));
    EXPECT_LT(at(md, "### 2026-07-24 Fixed"),
              at(md, "A layman explanation of the fix."));
    EXPECT_LT(at(md, "A layman explanation of the fix."),
              at(md, "- **The change.** (PROJ-9)"));
}

// INV-2 — a non-canonical category is refused.
TEST(changelog_log_subsection, Inv2BadCategory) {
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(kFlat), QStringLiteral("2026-07-24"),
        QStringLiteral("Bogus"), QStringLiteral("H"), QString(), {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_category"));
    EXPECT_TRUE(r.markdown.isEmpty());
}

// INV-3 — a body with no [Unreleased] heading is refused.
TEST(changelog_log_subsection, Inv3NotUnreleased) {
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8("# Changelog\n\n## [0.1.0]\n\n- x.\n"),
        QStringLiteral("2026-07-24"), QStringLiteral("Added"),
        QStringLiteral("H"), QString(), {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("not_unreleased"));
}

// INV-4 — newest-first: the new block lands ABOVE the existing dated topic,
// and the earlier subsection + its bullet survive.
TEST(changelog_log_subsection, Inv4NewestFirst) {
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(kFlat), QStringLiteral("2026-07-24"),
        QStringLiteral("Added"), QStringLiteral("Newer topic"), QString(), {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const std::string md = r.markdown.toStdString();
    EXPECT_LT(at(md, "### 2026-07-24 Added — Newer topic"),
              at(md, "### 2026-07-20 Added — Existing dated topic (PROJ-1)"));
    // The earlier entry + its bullet + the released section are intact.
    EXPECT_TRUE(contains(md, "An earlier entry."));
    EXPECT_TRUE(contains(md, "- **An earlier bullet.**"));
    EXPECT_TRUE(contains(md, "## [0.1.0] - 2026-01-01"));
}

// INV-5 — a missing blank spacer after [Unreleased] is repaired.
TEST(changelog_log_subsection, Inv5RepairsMissingSpacer) {
    const char *noSpacer =
        "# Changelog\n\n"
        "## [Unreleased]\n"
        "### 2026-07-20 Added — Prior (PROJ-1)\n\n"
        "- **b.**\n";
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(noSpacer), QStringLiteral("2026-07-24"),
        QStringLiteral("Fixed"), QStringLiteral("Topic"), QString(), {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    // A blank line now separates the heading from the new subsection.
    EXPECT_TRUE(contains(r.markdown.toStdString(),
                         "## [Unreleased]\n\n### 2026-07-24 Fixed — Topic"));
}

// INV-6 — no body and no bullets → just the heading + one blank line.
TEST(changelog_log_subsection, Inv6HeadingOnly) {
    const char *minimal =
        "# Changelog\n\n## [Unreleased]\n\n## [0.1.0]\n\n- old.\n";
    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(minimal), QStringLiteral("2026-07-24"),
        QStringLiteral("Changed"), QStringLiteral("Just a heading"),
        QString(), {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(contains(
        r.markdown.toStdString(),
        "## [Unreleased]\n\n### 2026-07-24 Changed — Just a heading\n\n"
        "## [0.1.0]"));
}

// INV-7 — handler write path rewrites CHANGELOG.md and reports the block.
TEST(changelog_log_subsection, Inv7HandlerWrites) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kFlat)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_subsection");
    req[QStringLiteral("category")]   = QStringLiteral("Added");
    req[QStringLiteral("date")]       = QStringLiteral("2026-07-24");
    req[QStringLiteral("headline")]   = QStringLiteral("Shiny feature (PROJ-9)");
    req[QStringLiteral("body")]       = QStringLiteral("What it does for you.");
    QJsonArray bullets;
    bullets.append(bullet(QStringLiteral("The wiring."),
                          QStringLiteral("under the hood."),
                          QStringLiteral("PROJ-9")));
    req[QStringLiteral("bullets")]    = bullets;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("category")).toString(),
              QStringLiteral("Added"));
    EXPECT_EQ(resp.value(QStringLiteral("date")).toString(),
              QStringLiteral("2026-07-24"));
    EXPECT_GT(resp.value(QStringLiteral("bytes_written")).toInt(), 0);
    const std::string md = readFileStd(clPath(tmp.path()));
    EXPECT_TRUE(contains(md, "### 2026-07-24 Added — Shiny feature (PROJ-9)"));
    EXPECT_TRUE(contains(md, "What it does for you."));
    EXPECT_TRUE(contains(md, "- **The wiring.** (PROJ-9)"));
    // Landed above the pre-existing dated topic (newest-first).
    EXPECT_LT(at(md, "### 2026-07-24 Added"), at(md, "### 2026-07-20 Added"));
}

// INV-8 — handler dry_run writes nothing.
TEST(changelog_log_subsection, Inv8DryRunWritesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kFlat)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("add_subsection");
    req[QStringLiteral("category")]   = QStringLiteral("Fixed");
    req[QStringLiteral("headline")]   = QStringLiteral("A fix");
    req[QStringLiteral("dry_run")]    = true;
    const QJsonObject resp = rc.cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_FALSE(resp.value(QStringLiteral("written")).toBool());
    EXPECT_GT(resp.value(QStringLiteral("line")).toInt(), 0);
    // The file is byte-identical to the input.
    EXPECT_EQ(readFileStd(clPath(tmp.path())), std::string(kFlat));
}

// INV-9 — handler guards: missing headline, bad category, kind-derived category.
TEST(changelog_log_subsection, Inv9Guards) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(clPath(tmp.path()), QByteArray(kFlat)));
    RemoteControl rc(nullptr);
    auto call = [&](const QJsonObject &extra) {
        QJsonObject req = extra;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("op")]         = QStringLiteral("add_subsection");
        req[QStringLiteral("dry_run")]    = true;
        return rc.cmdChangelogLog(req).object();
    };

    // Absent headline → missing_field.
    QJsonObject noHead;
    noHead[QStringLiteral("category")] = QStringLiteral("Added");
    const auto r1 = call(noHead);
    EXPECT_FALSE(r1.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r1.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));

    // Bad category → bad_category.
    QJsonObject badCat;
    badCat[QStringLiteral("headline")] = QStringLiteral("H");
    badCat[QStringLiteral("category")] = QStringLiteral("Bogus");
    const auto r2 = call(badCat);
    EXPECT_FALSE(r2.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r2.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_category"));

    // category omitted + kind:"fix" → derived Fixed.
    QJsonObject fromKind;
    fromKind[QStringLiteral("headline")] = QStringLiteral("H");
    fromKind[QStringLiteral("kind")]     = QStringLiteral("fix");
    const auto r3 = call(fromKind);
    ASSERT_TRUE(r3.value(QStringLiteral("ok")).toBool())
        << r3.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(r3.value(QStringLiteral("category")).toString(),
              QStringLiteral("Fixed"));
}

// ANTS-4356 — the layout guard existed in ONE direction only, and one call
// through the unguarded direction permanently bricks a flat changelog.
//
// `op:"add"` and `op:"normalize"` both refuse `feature_grouped_section`
// against a feature-grouped [Unreleased]. Nothing guarded the reverse:
// add_subsection against a genuinely FLAT section returned ok:true and
// produced a MIXED one — a dated topic sitting above a flat `### Added`
// block under a single [Unreleased]. From that point `add` refuses
// (feature_grouped_section now matches) and `normalize` refuses too, so one
// call on a flat-layout project permanently disables the flat write path
// with ok:true and no warning.
//
// The asymmetry reads as accidental: the existing refusal exists to stop an
// entry landing as a sibling of the dated topics and breaking the house
// style, and this is the same breach in the other direction.
//
// NB the fixture above named `kFlat` is not flat — it already carries a dated
// topic, which is why every existing row here writes into a feature-grouped
// section and none of them exercised this.
TEST(changelog_log_subsection, Ants4356RefusesAGenuinelyFlatSection) {
    const char *reallyFlat =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **A flat Keep-a-Changelog entry.**\n\n"
        "## [0.1.0] - 2026-01-01\n\n"
        "- old.\n";

    const auto r = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(reallyFlat), QStringLiteral("2026-08-14"),
        QStringLiteral("Fixed"), QStringLiteral("A dated topic (PROJ-9)"),
        QString(), {});
    EXPECT_FALSE(r.ok)
        << "a dated topic must not be written into a FLAT section — the "
           "result is a mixed section that disables op:add and op:normalize "
           "for good";
    EXPECT_EQ(r.code, QStringLiteral("flat_section"));
    EXPECT_TRUE(r.markdown.isEmpty() ||
                r.markdown == QString::fromUtf8(reallyFlat))
        << "a refusal must not have written anything";

    // The control: an ALREADY feature-grouped section still accepts one, so
    // the guard has not simply disabled the op.
    const auto ok = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(kFlat), QStringLiteral("2026-08-14"),
        QStringLiteral("Fixed"), QStringLiteral("A dated topic (PROJ-9)"),
        QString(), {});
    EXPECT_TRUE(ok.ok) << ok.error.toStdString();

    // And an EMPTY [Unreleased] accepts one too — that is how a project
    // deliberately converting layouts does it, and refusing there would make
    // conversion impossible rather than merely guarded.
    const char *emptyUnreleased =
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "## [0.1.0] - 2026-01-01\n\n"
        "- old.\n";
    const auto fresh = ChangelogLog::insertUnreleasedSubsection(
        QString::fromUtf8(emptyUnreleased), QStringLiteral("2026-08-14"),
        QStringLiteral("Fixed"), QStringLiteral("First dated topic (PROJ-9)"),
        QString(), {});
    EXPECT_TRUE(fresh.ok) << fresh.error.toStdString();
}
