// ANTS-4949 / ANTS-4968 — op:"set_intro" and op:"set_preamble".
// Contract: tests/features/roadmap_log_set_intro/spec.md
//
// Behavioural, through roadmap_log itself: each case migrates a small
// markdown fixture into a sandboxed store and reads the rendered file back.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <string>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL machine-global store under XDG_DATA_HOME.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla\n"
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
    "qui officia deserunt mollit anim id est laborum. Sed ut perspiciatis unde\n"
    "omnis iste natus error sit voluptatem accusantium doloremque laudantium,\n"
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Wrong Project \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        "The old work intro.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
        "  Layman: A thing to do.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "## Later\n"
        "\n"
        // ANTS-5378 — a stored `table` element beside the intro.
        "| Phase | State |\n"
        "|---|---|\n"
        "| 1 | done |\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0008] **A later item.**\n"
        "  Layman: A later thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}



QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    qint64 projectId = 0;
    QString root;
    bool ok() {
        if (!tmp.isValid()) return false;
        root = seedMigrated(guard, tmp, &projectId);
        return !root.isEmpty();
    }
};


QJsonObject introReq(const QString &root, const QString &section, const QString &text) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    if (!section.isNull()) req[QStringLiteral("section")] = section;
    req[QStringLiteral("new_text")] = text;
    return req;
}

int countLinesStarting(const std::string &md, const std::string &prefix) {
    int n = 0;
    size_t at = 0;
    while (at < md.size()) {
        const size_t eol = md.find('\n', at);
        const std::string line = md.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.rfind(prefix, 0) == 0) ++n;
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    return n;
}

}  // namespace

TEST(RoadmapLogSetIntro, ReplacesVerbatimAndRenders) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("New work intro.\n  - an indented point\n")), false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_GT(resp.value(QStringLiteral("replaced_intro_chars")).toInt(), 0);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "## Work\n\nNew work intro.\n  - an indented point\n"))
        << "the intro was not stored verbatim:\n" << md;
    EXPECT_FALSE(has(md, "The old work intro."));
}

TEST(RoadmapLogSetIntro, HeadingLineRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("Text.\n## Sneaky\n")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_intro"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

// ANTS-5095 — a failed project lookup is a store failure, not an unregistered
// project. Told "no row", a caller re-runs roadmap_migrate on a project the
// store already holds, which cannot help.
TEST(RoadmapLogSetIntro, Ants5095StoreErrorIsNotReportedAsUnregistered) {
    Fx fx; ASSERT_TRUE(fx.ok());
    {
        auto store = openStore(RoadmapStore::Access::Bulk);
        ASSERT_TRUE(store);
        QSqlQuery q(store->db());
        ASSERT_TRUE(q.exec(QStringLiteral("ALTER TABLE project RENAME TO project_gone")))
            << q.lastError().text().toStdString();
    }
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("convert");
    req[QStringLiteral("dry_run")]    = true;
    const QJsonObject resp = rc.cmdRoadmapLog(req).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("project_not_registered"))
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("store_failed"))
        << QJsonDocument(resp).toJson().toStdString();
}

TEST(RoadmapLogSetIntro, MissingSectionNamesSetPreamble) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QString(), QStringLiteral("x")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("missing_field"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "set_preamble"))
        << "a caller who forgot `section` must not land on the preamble";
}

TEST(RoadmapLogSetIntro, UnknownSectionOffersCandidates) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("wrok"), QStringLiteral("x")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"));
    EXPECT_TRUE(resp.value(QStringLiteral("candidates")).toArray()
                    .contains(QJsonValue(QStringLiteral("work"))));
}

TEST(RoadmapLogSetIntro, DryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    QJsonObject req = introReq(fx.root, QStringLiteral("work"), QStringLiteral("Preview."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(req, false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

TEST(RoadmapLogSetPreamble, ReplacesTheTitle) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QString(),
                 QString::fromUtf8("# Right Project \xE2\x80\x94 Roadmap\n\nA fresh preamble.")),
        true).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_EQ(md.rfind("<!-- ants-roadmap-format: 1 -->", 0), 0u)
        << "the render must still open on the format marker";
    EXPECT_TRUE(has(md, "# Right Project \xE2\x80\x94 Roadmap\n\nA fresh preamble."));
    EXPECT_FALSE(has(md, "Wrong Project"));
    EXPECT_EQ(countLinesStarting(md, "# "), 1) << "two H1s in the published file";
    EXPECT_TRUE(has(md, "## Work")) << "the sections survived";
}

TEST(RoadmapLogSetPreamble, SecondTitleOrSubheadingRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    for (const char *t : {"# One\n# Two", "# One\n## Sub"}) {
        const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
            introReq(fx.root, QString(), QString::fromLatin1(t)), true).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_intro")) << t;
    }
}

// ANTS-4555 — every rendered file says it is generated, once, under the
// format marker, and a preamble that already carries the notice is not given
// a second copy.
TEST(RoadmapLogSetPreamble, Ants4555GeneratedNoticeAppearsOnce) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QString notice = QStringLiteral(
        "<!-- Generated from the Ants Terminal roadmap store. Edit it with "
        "roadmap_log; hand edits are discarded by the next write. -->");
    for (const QString &text : {QStringLiteral("# Demo — Roadmap"),
                                notice + QStringLiteral("\n\n# Demo — Roadmap")}) {
        const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
            introReq(fx.root, QString(), text), true).object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << QJsonDocument(resp).toJson().toStdString();
        const QString md = QString::fromUtf8(readAll(roadmapPath(fx.root)));
        const QStringList lines = md.split(QLatin1Char('\n'));
        ASSERT_GE(lines.size(), 2);
        EXPECT_TRUE(lines.at(0).contains(QStringLiteral("ants-roadmap-format")));
        // Line 1 when the render adds it; line 2 when a stored preamble
        // already carried it after a blank line.
        const int at = lines.indexOf(notice);
        EXPECT_TRUE(at == 1 || at == 2)
            << "the notice must sit just under the marker:\n"
            << lines.mid(0, 5).join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(md.count(QStringLiteral("Generated from the Ants Terminal roadmap store")), 1)
            << md.toStdString();
    }
}

// ------------------------------------------------------------- ANTS-5373 -----

namespace {
QJsonObject amendReq(const QString &root, const QString &oldText, const QString &newText) {
    QJsonObject req = introReq(root, QStringLiteral("work"), newText);
    req[QStringLiteral("op")]       = QStringLiteral("amend_intro");
    req[QStringLiteral("old_text")] = oldText;
    return req;
}
}  // namespace

// `####` and deeper are intro text, which is what migration stores; the
// import makes sections of `##` and `###` only.
TEST(RoadmapLogSetIntro, Ants5373DeepHeadingsAccepted) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("Goal.\n\n#### Phase 11A\n\n##### Camera Shake\n")),
        false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "## Work\n\nGoal.\n\n#### Phase 11A\n\n##### Camera Shake\n")) << md;
}

// amend_intro replaces one match and keeps the rest of the intro.
TEST(RoadmapLogSetIntro, Ants5373AmendIntroKeepsTheRest) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("First line.\n\n#### Phase\n\nPhase text.")), false)
                    .object().value(QStringLiteral("ok")).toBool());
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        amendReq(fx.root, QStringLiteral("First line."), QStringLiteral("Better line.")),
        false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(), QStringLiteral("amend_intro"));
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "## Work\n\nBetter line.\n\n#### Phase\n\nPhase text.\n")) << md;
    // ANTS-5474 — it counts what was replaced: the match, not the whole intro.
    EXPECT_EQ(resp.value(QStringLiteral("replaced_intro_chars")).toInt(),
              int(QStringLiteral("First line.").size()))
        << QJsonDocument(resp).toJson().toStdString();
}

// The match must be unique, and a refusal writes nothing.
TEST(RoadmapLogSetIntro, Ants5373AmendIntroNeedsOneMatch) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    const QJsonObject none = rc.cmdRoadmapLogSetIntroForTest(
        amendReq(fx.root, QStringLiteral("absent"), QStringLiteral("x")), false).object();
    EXPECT_EQ(none.value(QStringLiteral("code")).toString(),
              QStringLiteral("intro_match_not_found"));
    const QJsonObject many = rc.cmdRoadmapLogSetIntroForTest(
        amendReq(fx.root, QStringLiteral("o"), QStringLiteral("x")), false).object();
    EXPECT_EQ(many.value(QStringLiteral("code")).toString(),
              QStringLiteral("intro_match_ambiguous"));
    const QJsonObject noOld = rc.cmdRoadmapLogSetIntroForTest(
        amendReq(fx.root, QString(), QStringLiteral("x")), false).object();
    EXPECT_EQ(noOld.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

// A preview echoes the intro that replaced_intro_chars counts.
TEST(RoadmapLogSetIntro, Ants5373DryRunEchoesPreviousIntro) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req = introReq(fx.root, QStringLiteral("work"), QStringLiteral("Preview."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(req, false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("previous_intro")).toString(),
              QStringLiteral("The old work intro."));
}

// ------------------------------------------------------------- ANTS-5493 -----

// A `# comment` inside a fenced code block is code, not a heading. The import
// is fence-aware, so the guard must be too, or an intro holding a shell
// sample can never be edited (MAME_Curator feedback 2026-09-27).
TEST(RoadmapLogSetIntro, Ants5493FencedHashLinesAreNotHeadings) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QString intro = QStringLiteral(
        "How to propose.\n\n```bash\n# Allocate the next ID:\nnext-id\n```\n\n"
        "~~~\n## also code\n~~~\n");
    QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"), intro), false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "```bash\n# Allocate the next ID:\nnext-id\n```\n")) << md;

    // amend_intro over the same intro, touching an unrelated line.
    resp = rc.cmdRoadmapLogSetIntroForTest(
        amendReq(fx.root, QStringLiteral("How to propose."),
                 QStringLiteral("How to propose an item.")), false).object();
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    // Outside a fence the guard still holds.
    resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("```\ncode\n```\n# Real heading\n")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_intro"));
}

// ------------------------------------------------------------- ANTS-5378 -----

// set_intro replaces the intro only. A section's table is a separate element
// and stays, so the reply says what it kept, and warns when the new intro
// brings a table of its own (LocalWebServerManager feedback 2026-09-25).
TEST(RoadmapLogSetIntro, Ants5378KeptElementsAreReported) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("later"), QStringLiteral("Later intro.")),
        false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("kept_elements")).toObject()
                  .value(QStringLiteral("table")).toInt(), 1)
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(resp.contains(QStringLiteral("warnings")));

    resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("later"),
                 QStringLiteral("Later intro.\n\n| Phase | State |\n|---|---|\n| 1 | redone |")),
        false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray warns = resp.value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warns.size(), 1) << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(warns.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("intro_table_beside_stored_table"));

    // A section holding only items reports nothing kept.
    resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"), QStringLiteral("Work intro.")),
        false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("kept_elements")));
}

// ------------------------------------------------------------- ANTS-5369 -----

// A session hand-restoring the same lines after every write sees them
// discarded every time (UT_MonsterHunt: 25 writes, one six-line preamble).
// The second identical discard says so, and names the ops that store it.
TEST(RoadmapLogSetIntro, Ants5369RepeatedDiscardIsNamed) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const auto write = [&](const char *intro) {
        return rc.cmdRoadmapLogSetIntroForTest(
            introReq(fx.root, QStringLiteral("work"), QString::fromUtf8(intro)),
            false).object();
    };
    const auto handRestore = [&] {
        QString md = QString::fromUtf8(readAll(roadmapPath(fx.root)));
        const int title = md.indexOf(QStringLiteral("\n# "));
        const int eol = md.indexOf(QLatin1Char('\n'), title + 1);
        md.insert(eol + 1, QStringLiteral("\nA hand-restored preamble line.\n"));
        QFile f(roadmapPath(fx.root));
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               f.write(md.toUtf8()) > 0;
    };
    ASSERT_TRUE(write("First.").value(QStringLiteral("ok")).toBool());

    ASSERT_TRUE(handRestore());
    QJsonObject resp = write("Second.");
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    ASSERT_GT(resp.value(QStringLiteral("discarded_text_lines")).toInt(), 0)
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(resp.contains(QStringLiteral("discard_repeated")));

    ASSERT_TRUE(handRestore());
    resp = write("Third.");
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("discard_repeated")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("discard_repeated_hint")).toString()
                    .contains(QStringLiteral("set_preamble")));
}

// ------------------------------------------------------------- ANTS-5161 -----

// A registered project whose roadmap reads as a dialect the store does not
// serve was told "the store holds no row" by repair_trailers and
// backfill_dates, and pointed at a re-migration that cannot help (Vestige,
// 1,026 rows, github-task-list). All three store-only ops now ask the store,
// as render already did (ANTS-4802), and name the format and the served ones.
TEST(RoadmapLogSetIntro, Ants5161RegisteredButNotServedIsNamed) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("gfm"));
    ASSERT_TRUE(writeFile(rawRoot + QStringLiteral("/ROADMAP.md"),
        QByteArray("# Roadmap\n\n## 0.7.0\n\n- [x] A shipped task.\n- [ ] An open task.\n")));
    const QString root = QFileInfo(rawRoot).canonicalFilePath();
    {
        auto store = openStore(RoadmapStore::Access::Bulk);
        ASSERT_TRUE(store);
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        ASSERT_TRUE(disc) << err.toStdString();
        const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Gfm"),
                                                   QStringLiteral("gfm"));
        RoadmapMigrateLoad::Options opts;
        opts.changedAt   = QStringLiteral("2026-09-27T10:00:00Z");
        opts.projectRoot = root;
        ASSERT_TRUE(RoadmapMigrateLoad::load(*store, plan, opts).ok);
    }

    RemoteControl rc(nullptr);
    for (const char *op : {"repair_trailers", "backfill_dates", "render"}) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QLatin1String(op);
        req[QStringLiteral("dry_run")]    = true;
        const QJsonObject resp = rc.cmdRoadmapLog(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("unsupported_format")) << op;
        EXPECT_TRUE(resp.value(QStringLiteral("store_row_present")).toBool()) << op;
        EXPECT_EQ(resp.value(QStringLiteral("store_source_format")).toString(),
                  QStringLiteral("github-task-list")) << op;
        const QString err = resp.value(QStringLiteral("error")).toString();
        EXPECT_TRUE(err.contains(QStringLiteral("pass-headings"))) << op << ": " << err.toStdString();
        EXPECT_FALSE(err.contains(QStringLiteral("holds no row"))) << op;
    }

    // An unregistered root still reads project_not_registered.
    const QString bare = QDir(tmp.path()).filePath(QStringLiteral("bare"));
    ASSERT_TRUE(writeFile(bare + QStringLiteral("/ROADMAP.md"), QByteArray("# Roadmap\n\n## Work\n")));
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = QFileInfo(bare).canonicalFilePath();
    req[QStringLiteral("op")]         = QStringLiteral("repair_trailers");
    EXPECT_EQ(rc.cmdRoadmapLog(req).object().value(QStringLiteral("code")).toString(),
              QStringLiteral("project_not_registered"));
}

// ANTS-5523 — amend_preamble changes one phrase and keeps the rest, through
// roadmap_log's real dispatch.
TEST(RoadmapLogSetPreamble, Ants5523AmendPreambleKeepsTheRest) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_preamble");
    req[QStringLiteral("old_text")]   = QStringLiteral("Wrong Project");
    req[QStringLiteral("new_text")]   = QStringLiteral("Right Project");
    const QJsonObject resp = rc.cmdRoadmapLog(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(), QStringLiteral("amend_preamble"));
    EXPECT_EQ(resp.value(QStringLiteral("replaced_intro_chars")).toInt(), 13);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_EQ(md.rfind("<!-- ants-roadmap-format: 1 -->", 0), 0u);
    EXPECT_TRUE(has(md, "# Right Project \xE2\x80\x94 Roadmap"));
    EXPECT_FALSE(has(md, "Wrong Project"));
    EXPECT_TRUE(has(md, "Lorem ipsum")) << "the rest of the preamble is kept";

    req[QStringLiteral("old_text")] = QStringLiteral("Not in the preamble");
    EXPECT_EQ(rc.cmdRoadmapLog(req).object().value(QStringLiteral("code")).toString(),
              QStringLiteral("intro_match_not_found"));
}
