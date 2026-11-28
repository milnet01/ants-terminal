// ANTS-6641 — the smaller roadmap_query / roadmap_log fixes from the
// 2026-11-06 reviews. Contract: tests/features/roadmap_log_review_fixes/spec.md
//
// Behavioural, through the verbs. Every case points XDG_DATA_HOME into its own
// temporary folder first, so the store the handlers open is the case's own; a
// store case migrates its fixture into it before the verb runs.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "rootprovider.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <unistd.h>

namespace {

// Past kRoadmapMinParseableSize, below which the write paths do not trust an
// ants-v1 or GFM walk.
const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust a walk. Lorem ipsum dolor sit amet, consectetur\n"
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

QByteArray antsV1() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
         "## Work\n"
         "\n"
         "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
         "  Layman: A thing.\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n"
         "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
         "  Layman: Another thing.\n"
         "  Kind: fix.\n"
         "  Source: seed.\n"
         "\n";
    return b;
}

QByteArray gfm() {
    QByteArray b = "# Demo\n\n";
    b += kPad;
    b += "\n## Work\n\n"
         "- [ ] **DEMO-0001** First task.\n"
         "- [ ] **DEMO-0002** Second task.\n";
    return b;
}

const char *kPass =
    "# Project Passes\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 1.1 First pass\n"
    "- **Status**: in-progress\n"
    "\n"
    "#### Pass 1.2 Second pass\n"
    "- **Status**: todo\n"
    "\n"
    "#### Pass 1.5 Third pass\n"
    "- **Status**: todo\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body) == body.size();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store unless XDG_DATA_HOME was redirected first, as every case does.
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

// Writes `markdown` at <tmp>/proj/ROADMAP.md and returns the canonical root;
// with `migrate`, loads it into the sandboxed store.
QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
             const QByteArray &markdown, bool migrate, qint64 *projectId = nullptr) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), markdown))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();
    if (!migrate) return root;
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
    if (projectId) *projectId = out.projectId;
    return root;
}

std::optional<RoadmapStore::ItemWrite> itemOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return std::nullopt;
    return store->readItem(*pk, &err);
}

QJsonObject call(const QString &root, const QString &op) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    if (!op.isEmpty()) r[QStringLiteral("op")] = op;
    return r;
}

QJsonObject logOf(const QJsonObject &req) {
    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    return rc.cmdRoadmapLog(req).object();
}

QJsonObject queryOf(const QJsonObject &req) {
    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    return rc.cmdRoadmapQueryForTest(req).object();
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

QString codeOf(const QJsonObject &o) { return o.value(QStringLiteral("code")).toString(); }

const QString kLeaky = QStringLiteral("Leaky.<parameter name=\"lanes\">a</parameter>");

bool reportsLanesScrub(const QJsonObject &o) {
    return o.value(QStringLiteral("note_scrubbed_params")).toArray()
            .contains(QJsonValue(QStringLiteral("lanes")));
}

}  // namespace

// INV-1 — row 97.
TEST(RoadmapLogReviewFixes, Row97NestedQuantifierRefuses) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), /*migrate=*/false);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject q = call(root, QString());
    q[QStringLiteral("query")] = QStringLiteral("(a+)+b");
    q[QStringLiteral("regex")] = true;
    const QJsonObject r1 = queryOf(q);
    EXPECT_FALSE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    EXPECT_EQ(codeOf(r1), QStringLiteral("bad_args")) << dump(r1);

    QJsonObject b = call(root, QString());
    b[QStringLiteral("body_match")]       = QStringLiteral("(x|y)+z");
    b[QStringLiteral("body_match_regex")] = true;
    const QJsonObject r2 = queryOf(b);
    EXPECT_FALSE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_EQ(codeOf(r2), QStringLiteral("bad_args")) << dump(r2);
}

// INV-2 — rows 97 and 107.
TEST(RoadmapLogReviewFixes, Row107BodyMatchEchoIsCappedAndScrubbed) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), false);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject longReq = call(root, QString());
    longReq[QStringLiteral("body_match")] = QString(300, QLatin1Char('q'));
    const QJsonObject r1 = queryOf(longReq);
    ASSERT_TRUE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    EXPECT_EQ(r1.value(QStringLiteral("body_match")).toString().size(), 200) << dump(r1);

    QJsonObject ctl = call(root, QString());
    ctl[QStringLiteral("body_match")] = QStringLiteral("thing\x01");
    const QJsonObject r2 = queryOf(ctl);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_EQ(r2.value(QStringLiteral("body_match")).toString(), QStringLiteral("thing?"))
        << dump(r2);
}

// INV-3 — row 98, both backends.
TEST(RoadmapLogReviewFixes, Row98HeadlineNewTextLineBreakRefuses) {
    for (const bool migrate : {true, false}) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seed(guard, tmp, antsV1(), migrate);
        ASSERT_FALSE(root.isEmpty());

        QJsonObject r = call(root, QStringLiteral("amend_headline"));
        r[QStringLiteral("id")]       = QStringLiteral("DEMO-0007");
        r[QStringLiteral("old_text")] = QStringLiteral("open item");
        r[QStringLiteral("new_text")] =
            QStringLiteral("open item.**\n- [DEMO-0099] **Injected");
        r[QStringLiteral("dry_run")]  = true;
        const QJsonObject resp = logOf(r);
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
            << (migrate ? "store: " : "file: ") << dump(resp);
        EXPECT_EQ(codeOf(resp), QStringLiteral("bad_args"))
            << (migrate ? "store: " : "file: ") << dump(resp);
    }
}

// INV-4 — row 101.
TEST(RoadmapLogReviewFixes, Row101PassFileAppendRefusesATakenDesignator) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, QByteArray(kPass), false);
    ASSERT_FALSE(root.isEmpty());
    const auto base = [&](const QString &op) {
        QJsonObject r = call(root, op);
        r[QStringLiteral("section")] = QStringLiteral("active");
        r[QStringLiteral("status")]  = QStringLiteral("planned");
        return r;
    };

    QJsonObject taken = base(QStringLiteral("append"));
    taken[QStringLiteral("pass")]     = QStringLiteral("1.1");
    taken[QStringLiteral("headline")] = QStringLiteral("A second 1.1.");
    const QJsonObject r1 = logOf(taken);
    EXPECT_FALSE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    EXPECT_EQ(codeOf(r1), QStringLiteral("id_taken")) << dump(r1);

    QJsonObject preview = base(QStringLiteral("append"));
    preview[QStringLiteral("pass")]     = QStringLiteral("1.3");
    preview[QStringLiteral("headline")] = QStringLiteral("New pass.");
    preview[QStringLiteral("dry_run")]  = true;
    const QJsonObject r2 = logOf(preview);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_EQ(r2.value(QStringLiteral("would_be_id")).toString(), QStringLiteral("PASS-1-3"))
        << dump(r2);
    EXPECT_FALSE(r2.contains(QStringLiteral("id"))) << dump(r2);

    QJsonObject batch = base(QStringLiteral("append_batch"));
    QJsonObject a, b;
    a[QStringLiteral("pass")] = QStringLiteral("1.2");
    a[QStringLiteral("headline")] = QStringLiteral("Taken.");
    b[QStringLiteral("pass")] = QStringLiteral("1.4");
    b[QStringLiteral("headline")] = QStringLiteral("Free.");
    batch[QStringLiteral("bullets")] = QJsonArray{a, b};
    const QJsonObject r3 = logOf(batch);
    ASSERT_TRUE(r3.value(QStringLiteral("ok")).toBool()) << dump(r3);
    EXPECT_EQ(r3.value(QStringLiteral("applied_count")).toInt(), 1) << dump(r3);
    const QJsonArray skipped = r3.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1) << dump(r3);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("id_taken")) << dump(r3);
}

// INV-4 — row 101: two bullets in one batch naming one pass, on both backends.
TEST(RoadmapLogReviewFixes, Row101PassBatchRefusesAnInBatchDuplicate) {
    for (const bool migrate : {false, true}) {
        const char *backend = migrate ? "store: " : "file: ";
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seed(guard, tmp, QByteArray(kPass), migrate);
        ASSERT_FALSE(root.isEmpty());

        QJsonObject batch = call(root, QStringLiteral("append_batch"));
        batch[QStringLiteral("section")] = QStringLiteral("active");
        batch[QStringLiteral("status")]  = QStringLiteral("planned");
        batch[QStringLiteral("pass")]    = QStringLiteral("1.6");
        QJsonObject a, b;
        a[QStringLiteral("headline")] = QStringLiteral("First 1.6.");
        b[QStringLiteral("headline")] = QStringLiteral("Second 1.6.");
        batch[QStringLiteral("bullets")] = QJsonArray{a, b};
        const QJsonObject r = logOf(batch);
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << backend << dump(r);
        EXPECT_EQ(r.value(QStringLiteral("applied_count")).toInt(), 1) << backend << dump(r);
        const QJsonArray skipped = r.value(QStringLiteral("skipped")).toArray();
        ASSERT_EQ(skipped.size(), 1) << backend << dump(r);
        EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("bullet_index")).toInt(), 1)
            << backend << dump(r);
        EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
                  QStringLiteral("id_taken")) << backend << dump(r);

        QFile f(root + QStringLiteral("/ROADMAP.md"));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_EQ(QString::fromUtf8(f.readAll()).count(QStringLiteral("#### Pass 1.6 ")), 1)
            << backend << "one designator, one heading";
    }
}

// INV-16 — row 118: an id_hint more than 10^6 above the high-water (7 here)
// refuses bad_args on append and skips bad_args on append_batch; one exactly
// 10^6 above is accepted. Both backends.
TEST(RoadmapLogReviewFixes, Row118IdHintFarAboveTheHighWaterRefuses) {
    for (const bool migrate : {false, true}) {
        const char *backend = migrate ? "store: " : "file: ";
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seed(guard, tmp, antsV1(), migrate);
        ASSERT_FALSE(root.isEmpty());
        if (!migrate)
            ASSERT_TRUE(writeFile(root + QStringLiteral("/.roadmap-counter"), "7\n"));

        const auto bullet = [] {
            QJsonObject b;
            b[QStringLiteral("status")]   = QStringLiteral("planned");
            b[QStringLiteral("headline")] = QStringLiteral("Hinted.");
            b[QStringLiteral("kind")]     = QStringLiteral("fix");
            b[QStringLiteral("source")]   = QStringLiteral("test");
            b[QStringLiteral("layman")]   = QStringLiteral("A hint check.");
            return b;
        };
        const auto append = [&](qint64 hint) {
            QJsonObject r = bullet();
            r[QStringLiteral("caller_cwd")] = root;
            r[QStringLiteral("op")]         = QStringLiteral("append");
            r[QStringLiteral("section")]    = QStringLiteral("work");
            r[QStringLiteral("id_hint")]    = hint;
            r[QStringLiteral("dry_run")]    = true;
            return logOf(r);
        };
        const auto appendBatch = [&](qint64 hint) {
            QJsonObject r = call(root, QStringLiteral("append_batch"));
            r[QStringLiteral("section")] = QStringLiteral("work");
            r[QStringLiteral("dry_run")] = true;
            QJsonObject b = bullet();
            b[QStringLiteral("id_hint")] = hint;
            r[QStringLiteral("bullets")] = QJsonArray{b};
            return logOf(r);
        };

        const qint64 tooHigh = 7 + 1000001;
        const QJsonObject r1 = append(tooHigh);
        EXPECT_FALSE(r1.value(QStringLiteral("ok")).toBool()) << backend << dump(r1);
        EXPECT_EQ(codeOf(r1), QStringLiteral("bad_args")) << backend << dump(r1);

        const QJsonObject r2 = append(tooHigh - 1);
        EXPECT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << backend << dump(r2);

        const QJsonObject r3 = appendBatch(tooHigh);
        // A dry run reports would_apply_count; applied_count is always 0.
        EXPECT_EQ(r3.value(QStringLiteral("would_apply_count")).toInt(), 0) << backend << dump(r3);
        const QJsonArray skipped = r3.value(QStringLiteral("skipped")).toArray();
        ASSERT_EQ(skipped.size(), 1) << backend << dump(r3);
        EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args")) << backend << dump(r3);

        const QJsonObject r4 = appendBatch(tooHigh - 1);
        EXPECT_EQ(r4.value(QStringLiteral("would_apply_count")).toInt(), 1) << backend << dump(r4);
    }
}

// INV-5 — row 102.
TEST(RoadmapLogReviewFixes, Row102LanesAreSanitised) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, antsV1(), true, &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject add = call(root, QStringLiteral("append"));
    add[QStringLiteral("section")]  = QStringLiteral("work");
    add[QStringLiteral("status")]   = QStringLiteral("planned");
    add[QStringLiteral("headline")] = QStringLiteral("Lanes are sanitised.");
    add[QStringLiteral("kind")]     = QStringLiteral("fix");
    add[QStringLiteral("source")]   = QStringLiteral("test");
    add[QStringLiteral("layman")]   = QStringLiteral("A lane check.");
    add[QStringLiteral("lanes")]    = QJsonArray{QStringLiteral("one\ntwo"),
                                                 QStringLiteral("three,four")};
    const QJsonObject r1 = logOf(add);
    ASSERT_TRUE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    const auto added = itemOf(r1.value(QStringLiteral("id")).toString(), projectId);
    ASSERT_TRUE(added.has_value()) << dump(r1);
    EXPECT_EQ(added->lanes, (QStringList{QStringLiteral("one two"),
                                         QStringLiteral("three four")}));

    QJsonObject amend = call(root, QStringLiteral("amend_field"));
    amend[QStringLiteral("id")]    = QStringLiteral("DEMO-0007");
    amend[QStringLiteral("field")] = QStringLiteral("lanes");
    amend[QStringLiteral("value")] = QJsonArray{QStringLiteral("five\nsix")};
    const QJsonObject r2 = logOf(amend);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    const auto amended = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(amended.has_value());
    EXPECT_EQ(amended->lanes, QStringList{QStringLiteral("five six")});
}

// INV-6 — row 104.
TEST(RoadmapLogReviewFixes, Row104BatchBodyEditsReportTheScrub) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), true);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject note = call(root, QStringLiteral("annotate"));
    note[QStringLiteral("id")]   = QStringLiteral("DEMO-0007");
    note[QStringLiteral("note")] = QStringLiteral("Body line to edit.");
    const QJsonObject r0 = logOf(note);
    ASSERT_TRUE(r0.value(QStringLiteral("ok")).toBool()) << dump(r0);

    QJsonObject batch = call(root, QStringLiteral("amend_batch"));
    batch[QStringLiteral("locators")] = QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("DEMO-0007")},
        {QStringLiteral("old_text"), QStringLiteral("Body line")},
        {QStringLiteral("new_text"), kLeaky}}};
    batch[QStringLiteral("dry_run")] = true;
    const QJsonObject r1 = logOf(batch);
    ASSERT_TRUE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    const QJsonArray rows = r1.value(QStringLiteral("amended")).toArray();
    ASSERT_EQ(rows.size(), 1) << dump(r1);
    EXPECT_TRUE(reportsLanesScrub(rows.at(0).toObject())) << dump(r1);

    QJsonObject replace = call(root, QStringLiteral("replace_text"));
    replace[QStringLiteral("old_text")] = QStringLiteral("Body line");
    replace[QStringLiteral("new_text")] = kLeaky;
    replace[QStringLiteral("dry_run")]  = true;
    const QJsonObject r2 = logOf(replace);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_TRUE(reportsLanesScrub(r2)) << dump(r2);
}

// INV-7 — row 105.
TEST(RoadmapLogReviewFixes, Row105ShortNoteInsideALineIsStillAppended) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, antsV1(), true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const auto annotate = [&](const QString &text) {
        QJsonObject r = call(root, QStringLiteral("annotate"));
        r[QStringLiteral("id")]   = QStringLiteral("DEMO-0007");
        r[QStringLiteral("note")] = text;
        return logOf(r);
    };

    const QJsonObject r1 = annotate(QStringLiteral("Checked the thing twice."));
    ASSERT_TRUE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    const QJsonObject r2 = annotate(QStringLiteral("Checked"));
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_TRUE(r2.value(QStringLiteral("note_appended")).toBool()) << dump(r2);
    EXPECT_FALSE(r2.contains(QStringLiteral("note_already_present"))) << dump(r2);
    const QStringList lines =
        itemOf(QStringLiteral("DEMO-0007"), projectId)->body.split(QLatin1Char('\n'));
    EXPECT_TRUE(lines.contains(QStringLiteral("Checked"))) << lines.join(QLatin1Char('|')).toStdString();

    // A whole-line repeat is still the retry it always was.
    const QJsonObject r3 = annotate(QStringLiteral("Checked the thing twice."));
    ASSERT_TRUE(r3.value(QStringLiteral("ok")).toBool()) << dump(r3);
    EXPECT_TRUE(r3.value(QStringLiteral("note_already_present")).toBool()) << dump(r3);
}

// INV-8 — row 106.
TEST(RoadmapLogReviewFixes, Row106SameStatusFlipTouchesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, antsV1(), true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const auto before = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(before.has_value());
    std::optional<qint64> historyBefore;
    if (auto s = openStore(RoadmapStore::Access::Interactive)) historyBefore = s->historyBytes();
    ASSERT_TRUE(historyBefore.has_value());

    QJsonObject r = call(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("DEMO-0007");
    r[QStringLiteral("to_status")] = QStringLiteral("planned");
    const QJsonObject resp = logOf(r);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);

    const auto after = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->lastModified, before->lastModified)
        << "an item nothing touched must not read as modified today";
    std::optional<qint64> historyAfter;
    if (auto s = openStore(RoadmapStore::Access::Interactive)) historyAfter = s->historyBytes();
    EXPECT_EQ(historyAfter.value_or(-1), *historyBefore)
        << "a same-status flip recorded a history row";
}

// INV-9 — row 108.
TEST(RoadmapLogReviewFixes, Row108WriteFailedDoesNotRestoreThePlan) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores the read-only directory";
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), true);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject preview = call(root, QStringLiteral("annotate"));
    preview[QStringLiteral("id")]      = QStringLiteral("DEMO-0007");
    preview[QStringLiteral("note")]    = QStringLiteral("Applied through a plan.");
    preview[QStringLiteral("dry_run")] = true;
    const QJsonObject dry = rc.cmdRoadmapLog(preview).object();
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool()) << dump(dry);
    const QString planId = dry.value(QStringLiteral("plan_id")).toString();
    ASSERT_FALSE(planId.isEmpty()) << dump(dry);

    QJsonObject apply = call(root, QString());
    apply[QStringLiteral("plan_id")] = planId;
    // The store commits; the render cannot publish into a read-only root.
    const auto rw = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    ASSERT_TRUE(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const QJsonObject failed = rc.cmdRoadmapLog(apply).object();
    QFile::setPermissions(root, rw);
    ASSERT_EQ(codeOf(failed), QStringLiteral("write_failed")) << dump(failed);

    const QJsonObject again = rc.cmdRoadmapLog(apply).object();
    EXPECT_EQ(codeOf(again), QStringLiteral("plan_not_found"))
        << "the store already committed this plan; " << dump(again);
}

// INV-10 — row 109.
TEST(RoadmapLogReviewFixes, Row109StorePassFlipLocatesByIdInTheStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, QByteArray(kPass), true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(itemOf(QStringLiteral("PASS-1-5"), projectId).has_value());

    // The file falls behind the store: Pass 1.5 is gone from it.
    QByteArray behind(kPass);
    behind.truncate(behind.indexOf("\n#### Pass 1.5"));
    behind += "\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), behind));

    QJsonObject r = call(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-1-5");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = logOf(r);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const auto item = itemOf(QStringLiteral("PASS-1-5"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, QStringLiteral("shipped"));
}

// INV-11 — row 111.
TEST(RoadmapLogReviewFixes, Row111SectionMoveCapsLocators) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), true);
    ASSERT_FALSE(root.isEmpty());

    QJsonArray locators;
    for (int i = 0; i < 501; ++i)
        locators.append(QJsonObject{{QStringLiteral("id"),
                                     QStringLiteral("DEMO-%1").arg(1000 + i)}});
    QJsonObject r = call(root, QStringLiteral("amend_field"));
    r[QStringLiteral("field")]    = QStringLiteral("section");
    r[QStringLiteral("value")]    = QStringLiteral("work");
    r[QStringLiteral("locators")] = locators;
    const QJsonObject resp = logOf(r);
    EXPECT_EQ(codeOf(resp), QStringLiteral("bad_args")) << dump(resp);
    EXPECT_TRUE(resp.value(QStringLiteral("error")).toString().contains(
        QStringLiteral("at most 500"))) << dump(resp);
}

// INV-12 — rows 1381 and 1382.
TEST(RoadmapLogReviewFixes, Row1381FilePathFlipDryRunCarriesTheEcho) {
    for (const bool isGfm : {false, true}) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seed(guard, tmp, isGfm ? gfm() : antsV1(), false);
        ASSERT_FALSE(root.isEmpty());

        QJsonObject r = call(root, QStringLiteral("flip"));
        r[QStringLiteral("id")]        = isGfm ? QStringLiteral("DEMO-0001")
                                               : QStringLiteral("DEMO-0007");
        r[QStringLiteral("to_status")] = QStringLiteral("shipped");
        r[QStringLiteral("note")]      = kLeaky;
        r[QStringLiteral("return")]    = QStringLiteral("headline_only");
        r[QStringLiteral("dry_run")]   = true;
        const QJsonObject resp = logOf(r);
        const char *which = isGfm ? "gfm: " : "ants-v1: ";
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << which << dump(resp);
        EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool()) << which << dump(resp);
        EXPECT_TRUE(reportsLanesScrub(resp)) << which << dump(resp);
        EXPECT_EQ(resp.value(QStringLiteral("post_bullets")).toArray().size(), 1)
            << which << dump(resp);
        EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
                  isGfm ? QStringLiteral("gfm") : QStringLiteral("ants-v1"))
            << which << dump(resp);
    }
}

// INV-13 — row 1384.
TEST(RoadmapLogReviewFixes, Row1384AmendBodyFileMissSuggests) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), false);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = call(root, QStringLiteral("amend_body"));
    r[QStringLiteral("id")]       = QStringLiteral("DEMO-0009");
    r[QStringLiteral("old_text")] = QStringLiteral("thing");
    r[QStringLiteral("new_text")] = QStringLiteral("other thing");
    const QJsonObject resp = logOf(r);
    EXPECT_EQ(codeOf(resp), QStringLiteral("bullet_not_found")) << dump(resp);
    EXPECT_FALSE(resp.value(QStringLiteral("suggestions")).toArray().isEmpty())
        << "a sibling id shares the DEMO- prefix; " << dump(resp);
}

// INV-14 — row 1387.
TEST(RoadmapLogReviewFixes, Row1387BatchEntryRefusalNamesTheBatchOp) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, antsV1(), true);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = call(root, QStringLiteral("amend_field_batch"));
    r[QStringLiteral("amendments")] = QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("DEMO-0007")},
                    {QStringLiteral("field"), QStringLiteral("kind")},
                    {QStringLiteral("value"), QStringLiteral("fix")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("DEMO-0003")},
                    {QStringLiteral("field"), QStringLiteral("kind")}}};
    r[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = logOf(r);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const QJsonArray skipped = resp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1) << dump(resp);
    const QString error = skipped.at(0).toObject().value(QStringLiteral("error")).toString();
    EXPECT_TRUE(error.contains(QStringLiteral("op:\"amend_field_batch\""))) << dump(resp);
}

// INV-15 — row 1390 (ANTS-5923).
TEST(RoadmapLogReviewFixes, Row1390SubdirectoryCallerReadsTheRootDeclaration) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QByteArray doc = "# Roadmap\n\n";
    doc += kPad;
    doc += "\n## Systems\n\n"
           "- [ ] **AX1. Geometric ray-traced audio occlusion**\n"
           "- [ ] **BX2. Doppler on moving sources**\n";
    const QString root = seed(guard, tmp, doc, false);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/.ants/project.json"),
        R"json({"id_format": {"pattern": "^([A-Za-z]{1,4}\\d+)(?:\\.|$)"}})json"));
    // The walk up from a subfolder stops at the repository top, as in a
    // real clone; without a .git it never leaves the subfolder.
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/.git")));
    const QString sub = root + QStringLiteral("/src/deep");
    ASSERT_TRUE(QDir().mkpath(sub));

    const auto idsOf = [](const QJsonObject &resp) {
        QStringList ids;
        for (const auto &v : resp.value(QStringLiteral("bullets")).toArray())
            ids << v.toObject().value(QStringLiteral("id")).toString();
        return ids;
    };
    QJsonObject list = call(sub, QString());
    list[QStringLiteral("status")] = QStringLiteral("all");
    const QJsonObject r1 = queryOf(list);
    ASSERT_TRUE(r1.value(QStringLiteral("ok")).toBool()) << dump(r1);
    EXPECT_TRUE(idsOf(r1).contains(QStringLiteral("AX1"))) << "list path: " << dump(r1);

    QJsonObject section = call(sub, QString());
    section[QStringLiteral("section")] = QStringLiteral("systems");
    section[QStringLiteral("status")]  = QStringLiteral("all");
    const QJsonObject r2 = queryOf(section);
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool()) << dump(r2);
    EXPECT_TRUE(idsOf(r2).contains(QStringLiteral("AX1"))) << "section path: " << dump(r2);
}
