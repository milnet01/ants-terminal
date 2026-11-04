// ANTS-4513 — the feedback corpus in the roadmap store. INV-1 to INV-10 of
// docs/specs/ANTS-4513-feedback-corpus-store.md; see spec.md beside this file.
//
// FeedbackStore is driven directly over a temporary store where a case needs
// two connections (INV-7, INV-8). Every other case drives the verbs, whose
// store is RoadmapStore::defaultPath() — under the bundle main's XDG_DATA_HOME
// sandbox, never the live one.

#include "feedbackfile.h"
#include "feedbackstore.h"
#include "remotecontrol.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSemaphore>
#include <QTemporaryDir>

#include <thread>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

QString fixture(const char *name) {
    return QStringLiteral(ANTS_SOURCE_DIR "/tests/features/feedback_store/fixtures/")
           + QString::fromLatin1(name);
}

QByteArray readBytes(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && f.write(bytes) == bytes.size();
}

// A corpus file copied into `dir` under a feedback name.
QString copyIn(const QTemporaryDir &dir, const char *fixtureName,
               const QString &as = QStringLiteral("Fx_Ants_MCP_Feedback.md")) {
    const QString to = dir.path() + QLatin1Char('/') + as;
    return writeBytes(to, readBytes(fixture(fixtureName))) ? to : QString();
}

QString sha256Hex(const QByteArray &b) {
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}

bool sameRows(const QVector<RoadmapStore::FeedbackFindingRow> &a,
              const QVector<RoadmapStore::FeedbackFindingRow> &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        const auto &x = a.at(i), &y = b.at(i);
        if (x.ordinal != y.ordinal || x.heading != y.heading
            || x.headingLine != y.headingLine || x.entryHeading != y.entryHeading
            || x.filedOn != y.filedOn || x.triage != y.triage || x.ids != y.ids)
            return false;
    }
    return true;
}

// The verbs' store, opened as a second connection to read what they wrote.
struct LiveStore {
    RoadmapStore store{RoadmapStore::defaultPath()};
    LiveStore() { EXPECT_TRUE(store.open()); }
    std::optional<RoadmapStore::FeedbackReporter> row(const QString &file) {
        return store.feedbackReporter(FeedbackStore::exportKey(file));
    }
};

QJsonObject req(const QString &path, const QString &op) {
    QJsonObject r;
    r["path"] = path;
    r["caller_cwd"] = QFileInfo(path).absolutePath();
    if (!op.isEmpty()) r["op"] = op;
    return r;
}

QJsonObject appendReq(const QString &path, const QString &title) {
    QJsonObject r = req(path, "append_finding");
    r["date"] = "2026-11-04";
    r["findings"] = QJsonArray{QJsonObject{{"title", title}, {"what", "w"}}};
    return r;
}

// The four surviving write ops, each aimed at corpus_mixed.md's untriaged
// finding, as {name, request}.
QVector<QPair<QString, QJsonObject>> writeOps(const QString &path) {
    const QString h = QStringLiteral("### changelog_log has no op to amend or remove an existing entry's text");
    QJsonObject assign = req(path, "assign_id");
    assign["heading"] = h;
    assign["ids"] = QJsonArray{"ANTS-7001"};
    QJsonObject batch = req(path, "assign_id_batch");
    batch["assignments"] = QJsonArray{QJsonObject{{"heading", h}, {"closure", "dup"}}};
    QJsonObject title = req(path, "set_title");
    title["title"] = "Renamed";
    return {{"append_finding", appendReq(path, "Appended")},
            {"assign_id", assign}, {"assign_id_batch", batch}, {"set_title", title}};
}

QStringList headingsOf(const QVector<RoadmapStore::FeedbackFindingRow> &rows,
                       const QStringList &triages) {
    QStringList out;
    for (const auto &r : rows)
        if (triages.contains(r.triage)) out << r.heading;
    return out;
}

}  // namespace

// INV-1 — import loses nothing, and the export writes the stored bytes back
// exactly: no line-ending or trailing-newline normalisation.
TEST(FeedbackStore, Inv1ImportLosesNothing) {
    for (const char *name : {"corpus_mixed.md", "corpus_crlf_no_eol.md", "golden_create.md"}) {
        QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
        RoadmapStore store(dir.path() + "/s.sqlite");
        ASSERT_TRUE(store.open());
        const QString p = copyIn(dir, name);
        const QByteArray original = readBytes(p);
        ASSERT_FALSE(original.isEmpty()) << name;

        const FeedbackStore::Result r = FeedbackStore::load(store, p);
        ASSERT_TRUE(r.code.isEmpty()) << name << ": " << r.message.toStdString();
        EXPECT_TRUE(r.imported);
        EXPECT_EQ(r.content.toUtf8(), original) << name;
        EXPECT_EQ(readBytes(p), original) << "import must not rewrite the file";

        // Write it out: drop the file, then make the smallest real edit.
        ASSERT_TRUE(QFile::remove(p));
        const FeedbackStore::Result w = FeedbackStore::write(
            store, p, QString(), false,
            [](const QString &in, bool, QString *out) { *out = in + "\n"; return true; });
        ASSERT_TRUE(w.code.isEmpty()) << name << ": " << w.message.toStdString();
        EXPECT_EQ(readBytes(p), original + "\n") << name;
    }
}

// INV-2 — the rows classify as parse() does; a Note line's id is not an id.
TEST(FeedbackStore, Inv2RowsClassifyAsParseDoes) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.path() + "/s.sqlite");
    ASSERT_TRUE(store.open());
    const QString p = copyIn(dir, "corpus_mixed.md");
    const FeedbackStore::Result r = FeedbackStore::load(store, p);
    ASSERT_TRUE(r.code.isEmpty()) << r.message.toStdString();
    const auto rows = store.feedbackFindings(r.reporterId);
    ASSERT_TRUE(rows);

    const FeedbackFile::ParseResult pr = FeedbackFile::parse(r.content);
    QStringList deltaHeadings;
    for (const QString &line : pr.delta.split('\n'))
        if (line.startsWith(QStringLiteral("### "))) deltaHeadings << line;
    QStringList awaiting, untagged;
    for (const auto &a : pr.awaiting) awaiting << a.heading;
    for (const auto &s : pr.suspectedUntagged) untagged << s.heading;

    EXPECT_EQ(headingsOf(*rows, {"untriaged", "awaiting"}), deltaHeadings);
    EXPECT_EQ(headingsOf(*rows, {"awaiting"}), awaiting);
    EXPECT_EQ(headingsOf(*rows, {"untagged"}), untagged);
    EXPECT_EQ(headingsOf(*rows, {"mapped"}).size(), 3);
    EXPECT_EQ(headingsOf(*rows, {"closed"}).size(), 1);
    EXPECT_EQ(rows->size(), 7) << "prose and fenced `###` lines are not findings";

    for (const auto &row : *rows) {
        if (row.heading.contains("changelog_log")) {
            EXPECT_EQ(row.triage, "untriaged");
            EXPECT_TRUE(row.ids.isEmpty()) << "the Note line's ANTS-5246 is not an id";
            EXPECT_EQ(row.filedOn, "2026-09-25");
        }
        if (row.heading.contains("file_outline")) {
            EXPECT_EQ(row.ids, (QStringList{"ANTS-4090", "ANTS-4091"}));
            EXPECT_TRUE(row.entryHeading.startsWith("## 2026-08-05"));
        }
    }
}

// INV-3 and INV-5 — after every surviving write op the stored rows are a
// rebuild of the stored content, and the export is that content, hashed,
// mode 0444.
TEST(FeedbackStore, Inv3Inv5RowsAndExportFollowEveryWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = copyIn(dir, "corpus_mixed.md");
    RemoteControl rc(nullptr);
    for (const auto &[name, request] : writeOps(p)) {
        const QJsonObject env = rc.cmdFeedbackLog(request).object();
        ASSERT_TRUE(env.value("ok").toBool())
            << name.toStdString() << ": " << QJsonDocument(env).toJson().constData();
        LiveStore live;
        const auto row = live.row(p);
        ASSERT_TRUE(row) << name.toStdString();
        const auto rows = live.store.feedbackFindings(row->reporterId);
        ASSERT_TRUE(rows);
        EXPECT_TRUE(sameRows(*rows, FeedbackStore::rowsFor(row->content)))
            << name.toStdString();
        const QByteArray file = readBytes(p);
        EXPECT_EQ(file, row->content.toUtf8()) << name.toStdString();
        EXPECT_EQ(sha256Hex(file), row->exportSha256) << name.toStdString();
        const QFileDevice::Permissions mode = QFile::permissions(p);
        EXPECT_TRUE(mode.testFlags(QFileDevice::ReadOwner | QFileDevice::ReadGroup
                                   | QFileDevice::ReadOther))
            << name.toStdString();
        EXPECT_FALSE(mode & (QFileDevice::WriteOwner | QFileDevice::WriteGroup
                             | QFileDevice::WriteOther))
            << name.toStdString() << ": the export must be 0444";
    }
}

// INV-4 — feedback_query answers as parse() does on the file's bytes.
TEST(FeedbackStore, Inv4QueryAnswersAsParse) {
    for (const char *name : {"corpus_mixed.md", "corpus_crlf_no_eol.md", "golden_create.md"}) {
        QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
        const QString p = copyIn(dir, name);
        const FeedbackFile::ParseResult pr =
            FeedbackFile::parse(QString::fromUtf8(readBytes(p)));
        RemoteControl rc(nullptr);
        const QJsonObject env = rc.cmdFeedbackQuery(req(p, QString())).object();
        ASSERT_TRUE(env.value("ok").toBool()) << name;
        EXPECT_EQ(env.value("delta").toString(), pr.delta) << name;
        EXPECT_EQ(env.value("delta_present").toBool(), pr.deltaPresent) << name;
        EXPECT_EQ(env.value("delta_line_count").toInt(), pr.deltaLineCount) << name;
        EXPECT_EQ(env.value("delta_start_line").toInt(), pr.deltaStartLine) << name;
        QStringList ids;
        for (const auto &v : env.value("mapped_ids").toArray()) ids << v.toString();
        EXPECT_EQ(ids, pr.mappedIds) << name;
        EXPECT_EQ(env.value("maintainer_block_count").toInt(), pr.maintainerBlockCount);
        EXPECT_EQ(env.value("format_version").toInt(), pr.formatVersion);
        EXPECT_EQ(env.value("awaiting").toArray().size(), pr.awaiting.size());
        EXPECT_EQ(env.value("suspected_untagged").toArray().size(),
                  pr.suspectedUntagged.size());
        EXPECT_FALSE(env.contains("export_drift")) << name;
    }
}

// INV-6 — a hand-edited export: every write op refuses export_drift and
// changes nothing; reads say so; reimport settles it, with confirm only.
TEST(FeedbackStore, Inv6DriftRefusesAndChangesNothing) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = copyIn(dir, "corpus_mixed.md");
    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdFeedbackLog(appendReq(p, "First")).object().value("ok").toBool());

    const QByteArray edited = readBytes(p) + "\nA hand edit.\n";
    ASSERT_TRUE(writeBytes(p, edited));
    LiveStore before;
    const auto rowBefore = before.row(p);
    ASSERT_TRUE(rowBefore);
    const auto rowsBefore = before.store.feedbackFindings(rowBefore->reporterId);

    for (const auto &[name, request] : writeOps(p)) {
        const QJsonObject env = rc.cmdFeedbackLog(request).object();
        EXPECT_EQ(env.value("code").toString(), "export_drift") << name.toStdString();
        EXPECT_EQ(readBytes(p), edited) << name.toStdString();
        LiveStore live;
        const auto row = live.row(p);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->content, rowBefore->content) << name.toStdString();
        EXPECT_EQ(row->revision, rowBefore->revision) << name.toStdString();
        EXPECT_TRUE(sameRows(*live.store.feedbackFindings(row->reporterId), *rowsBefore));
    }
    EXPECT_TRUE(rc.cmdFeedbackQuery(req(p, QString())).object().value("export_drift").toBool());

    QJsonObject re = req(p, "reimport");
    const QJsonObject refused = rc.cmdFeedbackLog(re).object();
    EXPECT_EQ(refused.value("code").toString(), "confirm_required");
    EXPECT_EQ(refused.value("file_bytes").toInt(), edited.size());
    re["confirm"] = true;
    ASSERT_TRUE(rc.cmdFeedbackLog(re).object().value("ok").toBool());
    LiveStore after;
    EXPECT_EQ(after.row(p)->content.toUtf8(), edited);
    EXPECT_TRUE(rc.cmdFeedbackLog(appendReq(p, "After reimport")).object()
                    .value("ok").toBool());
}

// INV-7 — two appends from two connections, released together, both land.
TEST(FeedbackStore, Inv7ConcurrentAppendsBothLand) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString db = dir.path() + "/s.sqlite";
    { RoadmapStore s(db); ASSERT_TRUE(s.open()); }   // schema, before the race
    const QString p = copyIn(dir, "corpus_mixed.md");
    for (int round = 0; round < 5; ++round) {
        QSemaphore go;
        QString codes[2];
        auto writer = [&](int i) {
            RoadmapStore s(db);
            if (!s.open()) { codes[i] = "open"; go.acquire(); return; }
            go.acquire();
            const FeedbackStore::Result r = FeedbackStore::write(
                s, p, QString(), false, [&](const QString &in, bool, QString *out) {
                    *out = in + QStringLiteral("\nwriter-%1-%2\n").arg(round).arg(i);
                    return true;
                });
            codes[i] = r.code;
        };
        std::thread a(writer, 0), b(writer, 1);
        go.release(2);
        a.join();
        b.join();
        EXPECT_EQ(codes[0], QString()) << round;
        EXPECT_EQ(codes[1], QString()) << round;
        RoadmapStore s(db);
        ASSERT_TRUE(s.open());
        const QString content = s.feedbackReporter(FeedbackStore::exportKey(p))->content;
        const QByteArray file = readBytes(p);
        for (int i = 0; i < 2; ++i) {
            const QString mark = QStringLiteral("writer-%1-%2").arg(round).arg(i);
            EXPECT_TRUE(content.contains(mark)) << mark.toStdString();
            EXPECT_TRUE(file.contains(mark.toUtf8())) << mark.toStdString();
        }
    }
}

// INV-8 — a file is imported once: twice in sequence, then two first
// touches at once, each of which succeeds.
TEST(FeedbackStore, Inv8FileImportedOnce) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString db = dir.path() + "/s.sqlite";
    RoadmapStore store(db);
    ASSERT_TRUE(store.open());
    const QString p1 = copyIn(dir, "corpus_mixed.md", "One_Ants_MCP_Feedback.md");
    const auto first = FeedbackStore::load(store, p1);
    const auto second = FeedbackStore::load(store, p1);
    EXPECT_TRUE(first.imported);
    EXPECT_FALSE(second.imported);
    EXPECT_EQ(first.reporterId, second.reporterId);

    const QString p2 = copyIn(dir, "corpus_mixed.md", "Two_Ants_MCP_Feedback.md");
    QSemaphore go;
    FeedbackStore::Result results[2];
    auto toucher = [&](int i) {
        RoadmapStore s(db);
        if (!s.open()) { results[i].code = "open"; go.acquire(); return; }
        go.acquire();
        results[i] = FeedbackStore::load(s, p2);
    };
    std::thread a(toucher, 0), b(toucher, 1);
    go.release(2);
    a.join();
    b.join();
    EXPECT_EQ(results[0].code, QString());
    EXPECT_EQ(results[1].code, QString());
    EXPECT_EQ(int(results[0].imported) + int(results[1].imported), 1);
    EXPECT_EQ(results[0].reporterId, results[1].reporterId);
}

// INV-9 — a first filing registers its reporter and writes exactly what the
// file-backed create path wrote (golden made before the store change).
TEST(FeedbackStore, Inv9FirstFilingMatchesTheGolden) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/Golden_Ants_MCP_Feedback.md";
    QJsonObject r = req(p, "append_finding");
    r["date"] = "2026-11-04";
    r["session_label"] = "golden session";
    r["note"] = "Intro note.";
    r["findings"] = QJsonArray{
        QJsonObject{{"title", "First finding"}, {"what", "what text"},
                    {"repro", "line one\n# a hash line"}, {"impact", "impact text"},
                    {"suggested_fix", "fix text"}},
        QJsonObject{{"title", "Second finding"}, {"what", "only what"}}};
    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdFeedbackLog(r).object();
    ASSERT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().constData();
    EXPECT_TRUE(env.value("created").toBool());
    EXPECT_EQ(readBytes(p), readBytes(fixture("golden_create.md")));
    LiveStore live;
    EXPECT_TRUE(live.row(p));
}

// INV-10 — each retired op refuses `retired`; the file and the store are
// unchanged.
TEST(FeedbackStore, Inv10RetiredOpsWriteNothing) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = copyIn(dir, "corpus_mixed.md");
    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdFeedbackQuery(req(p, QString())).object().value("ok").toBool());
    const QByteArray file = readBytes(p);
    LiveStore before;
    const auto row = before.row(p);
    ASSERT_TRUE(row);
    for (const char *op : {"compact_resolved", "compact_shipped", "prune_tracking",
                           "append_tracking", "migrate_v2", "set_format_version"}) {
        const QJsonObject env = rc.cmdFeedbackLog(req(p, op)).object();
        EXPECT_EQ(env.value("code").toString(), "retired") << op;
        EXPECT_EQ(readBytes(p), file) << op;
        LiveStore live;
        EXPECT_EQ(live.row(p)->revision, row->revision) << op;
        EXPECT_EQ(live.row(p)->content, row->content) << op;
    }
}
