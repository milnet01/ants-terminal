// ANTS-5379 — roadmap_log element ops: list, amend, delete, promote.
// Contract: tests/features/roadmap_log_elements/spec.md
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
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
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
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    // ANTS-5615 — a status legend, which the migration stores on the project.
    b += "\n"
        "- \xF0\x9F\x93\x8B Planned (next up for this phase)\n"
        "- \xE2\x9C\x85 Done\n";
    b += "\n"
        "## Work\n"
        "\n"
        "The work intro.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
        "  Layman: A thing to do.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "A loose note about the work.\n"
        "\n"
        "## Later\n"
        "\n"
        "| Phase | State |\n"
        "|---|---|\n"
        "| 1 | done |\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0008] **A later item.**\n"
        "  Layman: A later thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "- Promote me to an item\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp) {
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
    opts.changedAt   = QStringLiteral("2026-09-28T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    return root;
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    QString root;
    bool ok() {
        if (!tmp.isValid()) return false;
        root = seedMigrated(guard, tmp);
        return !root.isEmpty();
    }
};

QJsonObject call(RemoteControl &rc, const QString &root, const QString &op,
                 QJsonObject req) {
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")] = op;
    return rc.cmdRoadmapLog(req).object();
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

QJsonArray listSection(RemoteControl &rc, const QString &root, const QString &slug) {
    const QJsonObject resp = call(rc, root, QStringLiteral("list_elements"),
                                  QJsonObject{{QStringLiteral("section"), slug}});
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    return resp.value(QStringLiteral("elements")).toArray();
}

// The position of the first element of `kind` whose text/id contains `needle`.
int positionOf(const QJsonArray &elems, const QString &kind, const QString &needle) {
    for (const QJsonValue &v : elems) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("kind")).toString() != kind) continue;
        const QString hay = e.value(QStringLiteral("text")).toString() +
                            e.value(QStringLiteral("id")).toString();
        if (hay.contains(needle)) return e.value(QStringLiteral("position")).toInt();
    }
    return -1;
}

}  // namespace

TEST(RoadmapLogElements, ListsInPositionOrder) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonArray work = listSection(rc, fx.root, QStringLiteral("work"));
    const int item = positionOf(work, QStringLiteral("item"), QStringLiteral("DEMO-0007"));
    const int note = positionOf(work, QStringLiteral("narration"),
                                QStringLiteral("A loose note about the work."));
    ASSERT_GE(item, 0) << QJsonDocument(work).toJson().toStdString();
    ASSERT_GE(note, 0) << QJsonDocument(work).toJson().toStdString();
    EXPECT_LT(item, note);
    int prev = -1;
    for (const QJsonValue &v : work) {
        const int p = v.toObject().value(QStringLiteral("position")).toInt();
        EXPECT_GT(p, prev) << "elements not in position order";
        prev = p;
    }
    const QJsonArray later = listSection(rc, fx.root, QStringLiteral("later"));
    bool sawTable = false;
    for (const QJsonValue &v : later) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("kind")).toString() == QStringLiteral("table")) {
            sawTable = true;
            EXPECT_EQ(e.value(QStringLiteral("rows")).toInt(), 1) << dump(e);
        }
    }
    EXPECT_TRUE(sawTable) << QJsonDocument(later).toJson().toStdString();
}

TEST(RoadmapLogElements, AmendReplacesNarration) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const int note = positionOf(listSection(rc, fx.root, QStringLiteral("work")),
                                QStringLiteral("narration"), QStringLiteral("loose note"));
    ASSERT_GE(note, 0);
    const QJsonObject resp = call(rc, fx.root, QStringLiteral("amend_element"),
        QJsonObject{{QStringLiteral("section"), QStringLiteral("work")},
                    {QStringLiteral("element_position"), note},
                    {QStringLiteral("new_text"), QStringLiteral("A corrected note.")}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "A corrected note.")) << md;
    EXPECT_FALSE(has(md, "A loose note about the work.")) << md;
}

TEST(RoadmapLogElements, AmendRefusals) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonArray work = listSection(rc, fx.root, QStringLiteral("work"));
    const int item = positionOf(work, QStringLiteral("item"), QStringLiteral("DEMO-0007"));
    const int note = positionOf(work, QStringLiteral("narration"), QStringLiteral("loose note"));
    ASSERT_GE(item, 0);
    ASSERT_GE(note, 0);
    const QByteArray before = readAll(roadmapPath(fx.root));

    const auto amend = [&](int pos, const QString &text) {
        return call(rc, fx.root, QStringLiteral("amend_element"),
            QJsonObject{{QStringLiteral("section"), QStringLiteral("work")},
                        {QStringLiteral("element_position"), pos},
                        {QStringLiteral("new_text"), text}});
    };
    EXPECT_EQ(amend(item, QStringLiteral("x")).value(QStringLiteral("code")).toString(),
              QStringLiteral("element_kind_refused"));
    EXPECT_EQ(amend(9999, QStringLiteral("x")).value(QStringLiteral("code")).toString(),
              QStringLiteral("element_not_found"));
    EXPECT_EQ(amend(note, QStringLiteral("## Not a section")).value(QStringLiteral("code"))
                  .toString(),
              QStringLiteral("bad_element_text"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before) << "a refused amend changed the file";
}

TEST(RoadmapLogElements, DeleteRemovesNarrationNotItems) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonArray work = listSection(rc, fx.root, QStringLiteral("work"));
    const int item = positionOf(work, QStringLiteral("item"), QStringLiteral("DEMO-0007"));
    const int note = positionOf(work, QStringLiteral("narration"), QStringLiteral("loose note"));
    ASSERT_GE(item, 0);
    ASSERT_GE(note, 0);
    const auto del = [&](int pos) {
        return call(rc, fx.root, QStringLiteral("delete_element"),
            QJsonObject{{QStringLiteral("section"), QStringLiteral("work")},
                        {QStringLiteral("element_position"), pos}});
    };
    EXPECT_EQ(del(item).value(QStringLiteral("code")).toString(),
              QStringLiteral("element_kind_refused"));
    const QJsonObject resp = del(note);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_TRUE(resp.value(QStringLiteral("removed_text")).toString()
                    .contains(QStringLiteral("loose note")));
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_FALSE(has(md, "A loose note about the work.")) << md;
    EXPECT_TRUE(has(md, "DEMO-0007")) << md;
    EXPECT_EQ(positionOf(listSection(rc, fx.root, QStringLiteral("work")),
                         QStringLiteral("narration"), QStringLiteral("loose note")), -1);
}

TEST(RoadmapLogElements, DeleteDryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const int note = positionOf(listSection(rc, fx.root, QStringLiteral("work")),
                                QStringLiteral("narration"), QStringLiteral("loose note"));
    ASSERT_GE(note, 0);
    const QByteArray before = readAll(roadmapPath(fx.root));
    const QJsonObject resp = call(rc, fx.root, QStringLiteral("delete_element"),
        QJsonObject{{QStringLiteral("section"), QStringLiteral("work")},
                    {QStringLiteral("element_position"), note},
                    {QStringLiteral("dry_run"), true}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
    EXPECT_GE(positionOf(listSection(rc, fx.root, QStringLiteral("work")),
                         QStringLiteral("narration"), QStringLiteral("loose note")), 0);
}

TEST(RoadmapLogElements, PromoteFilesItemInPlace) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonArray later = listSection(rc, fx.root, QStringLiteral("later"));
    const int note = positionOf(later, QStringLiteral("narration"), QStringLiteral("Promote me"));
    const int table = positionOf(later, QStringLiteral("table"), QString());
    ASSERT_GE(note, 0) << QJsonDocument(later).toJson().toStdString();
    ASSERT_GE(table, 0);

    QJsonObject fields{{QStringLiteral("section"), QStringLiteral("later")},
                       {QStringLiteral("status"), QStringLiteral("planned")},
                       {QStringLiteral("kind"), QStringLiteral("chore")},
                       {QStringLiteral("source"), QStringLiteral("test")},
                       {QStringLiteral("layman"), QStringLiteral("A promoted note")}};
    QJsonObject onTable = fields;
    onTable[QStringLiteral("element_position")] = table;
    EXPECT_EQ(call(rc, fx.root, QStringLiteral("promote_element"), onTable)
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("element_kind_refused"));

    QJsonObject req = fields;
    req[QStringLiteral("element_position")] = note;
    const QJsonObject resp = call(rc, fx.root, QStringLiteral("promote_element"), req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const QString id = resp.value(QStringLiteral("id")).toString();
    EXPECT_TRUE(id.startsWith(QStringLiteral("DEMO-"))) << dump(resp);

    const QJsonArray after = listSection(rc, fx.root, QStringLiteral("later"));
    EXPECT_EQ(positionOf(after, QStringLiteral("item"), id), note)
        << QJsonDocument(after).toJson().toStdString();
    EXPECT_EQ(positionOf(after, QStringLiteral("narration"), QStringLiteral("Promote me")), -1);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "**Promote me to an item")) << md;
}

// ANTS-5615 — the status legend is not an element, so list_elements names it
// with the op that reaches it, and set_legend changes one status's wording.
TEST(RoadmapLogElements, SetLegendChangesOneStatusWording) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const std::string before = readAll(roadmapPath(fx.root)).toStdString();
    ASSERT_TRUE(has(before, "Planned (next up for this phase)")) << before;

    const QJsonObject listed = call(rc, fx.root, QStringLiteral("list_elements"),
        QJsonObject{{QStringLiteral("preamble"), true}});
    ASSERT_TRUE(listed.value(QStringLiteral("ok")).toBool()) << dump(listed);
    EXPECT_EQ(listed.value(QStringLiteral("legend")).toObject()
                  .value(QStringLiteral("planned")).toString(),
              QStringLiteral("Planned (next up for this phase)")) << dump(listed);
    EXPECT_TRUE(listed.value(QStringLiteral("legend_hint")).toString()
                    .contains(QStringLiteral("set_legend"))) << dump(listed);

    const QJsonObject resp = call(rc, fx.root, QStringLiteral("set_legend"),
        QJsonObject{{QStringLiteral("legend"),
                     QJsonObject{{QStringLiteral("planned"),
                                  QStringLiteral("Planned (next up)")}}}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "\xF0\x9F\x93\x8B Planned (next up)\n")) << md;
    EXPECT_FALSE(has(md, "next up for this phase")) << md;
    EXPECT_TRUE(has(md, "\xE2\x9C\x85 Done")) << "an unnamed status lost its line\n" << md;
}

TEST(RoadmapLogElements, SetLegendRefusalsAndDryRunWriteNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QByteArray before = readAll(roadmapPath(fx.root));
    const auto setLegend = [&](const QJsonObject &legend, bool dryRun) {
        QJsonObject req{{QStringLiteral("legend"), legend}};
        if (dryRun) req[QStringLiteral("dry_run")] = true;
        return call(rc, fx.root, QStringLiteral("set_legend"), req);
    };
    EXPECT_EQ(setLegend(QJsonObject{{QStringLiteral("blocked"), QStringLiteral("Planned")}}, false)
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_EQ(setLegend(QJsonObject{{QStringLiteral("planned"), QStringLiteral("Next up")}}, false)
                  .value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args")) << "a wording the import cannot read back";
    EXPECT_EQ(setLegend(QJsonObject{}, false).value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
    const QJsonObject dry =
        setLegend(QJsonObject{{QStringLiteral("planned"), QStringLiteral("Planned soon")}}, true);
    EXPECT_TRUE(dry.value(QStringLiteral("ok")).toBool()) << dump(dry);
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before) << "a refusal or dry run changed the file";
}
