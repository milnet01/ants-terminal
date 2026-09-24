// Feature-conformance test for ANTS-5299 — the run_trace verb.
// Contract: tests/features/run_trace/spec.md.
//
// Every case drives the RunTraceVerb seam against a QTemporaryDir, with the
// clock, the random source and the state directory passed in. INV-14 alone is
// a source scrape, because the provider registry is private.

#include <gtest/gtest.h>

#include "runtraceverb.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>

#include <fstream>
#include <sstream>

namespace {

QString header() { return QStringLiteral("| Run | Date | Subject | Lanes | Outcome |"); }

struct Fixture {
    QTemporaryDir dir;
    RunTraceVerb::Paths paths;

    bool init() {
        if (!dir.isValid())
            return false;
        const QString root = dir.filePath(QStringLiteral("proj"));
        const QString state = dir.filePath(QStringLiteral("state"));
        if (!QDir().mkpath(root) || !QDir().mkpath(state))
            return false;
        paths.root = QDir(root).canonicalPath();
        paths.indexRel = QStringLiteral("docs/reviews/gate-log.md");
        paths.stateDir = state;
        return true;
    }
    QString index() const { return paths.root + QLatin1Char('/') + paths.indexRel; }
    QString at(const QString &rel) const { return paths.root + QLatin1Char('/') + rel; }
};

// Noon local time, so the local date is 2026-09-24 in every timezone.
QDateTime kNow() {
    return QDateTime(QDate(2026, 9, 24), QTime(12, 0, 0));
}

std::function<quint32()> seq(QList<quint32> values) {
    auto state = std::make_shared<QList<quint32>>(std::move(values));
    return [state]() -> quint32 {
        if (state->isEmpty())
            return 0xbeefu;
        return state->takeFirst();
    };
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

bool writeAll(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(text.toUtf8());
    return true;
}

// Every file under `dir`, path → content. Two snapshots comparing equal is
// "nothing was written".
QMap<QString, QString> snapshot(const QString &dir) {
    QMap<QString, QString> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        out.insert(p, readAll(p));
    }
    return out;
}

QString startId(Fixture &fx, const QString &kind = QStringLiteral("G"),
                const QString &subject = QStringLiteral("docs/specs/x.md")) {
    const QJsonObject r = RunTraceVerb::start(fx.paths, kind, subject, kNow(),
                                              false, seq({0xa3f1u}));
    return r.value(QStringLiteral("ok")).toBool()
               ? r.value(QStringLiteral("id")).toString() : QString();
}

QJsonObject finishWith(Fixture &fx, const QString &id, QJsonObject args,
                       bool dryRun = false, const QString &transcript = {}) {
    RunTraceVerb::FinishRequest req;
    req.id = id;
    req.args = std::move(args);
    req.transcriptPath = transcript;
    req.dryRun = dryRun;
    return RunTraceVerb::finish(fx.paths, req);
}

QJsonObject basicArgs(int lanes = 3) {
    QJsonObject a;
    a[QStringLiteral("lanes")] = lanes;
    a[QStringLiteral("outcome")] = QStringLiteral("complete, 2 findings fixed");
    a[QStringLiteral("detail")] = QStringLiteral("# Run\n\nLanes read it.\n");
    return a;
}

QString code(const QJsonObject &r) { return r.value(QStringLiteral("code")).toString(); }

// Local-time ISO stamp in the harness's shape.
QString utcStamp(const QDateTime &t) {
    return t.toUTC().toString(Qt::ISODateWithMs);
}

}  // namespace

// INV-1
TEST(RunTrace, StartMintsIdAndWritesNoTrackedFile) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const auto before = snapshot(fx.paths.root);
    const QJsonObject r = RunTraceVerb::start(fx.paths, QStringLiteral("C"),
                                              QStringLiteral("src/"), kNow(),
                                              false, seq({0x00a3u}));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << code(r).toStdString();
    const QString id = r.value(QStringLiteral("id")).toString();
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[GCVEFRAL]-\\d{8}-[0-9a-f]{4}$"))
                    .match(id).hasMatch()) << id.toStdString();
    EXPECT_EQ(id, QStringLiteral("C-20260924-00a3"));
    EXPECT_EQ(snapshot(fx.paths.root), before);
    EXPECT_TRUE(QFile::exists(fx.paths.stateDir + QLatin1Char('/') + id
                              + QStringLiteral(".json")));

    for (const QString &k : {QStringLiteral("X"), QStringLiteral(""),
                             QStringLiteral("GC")}) {
        EXPECT_EQ(code(RunTraceVerb::start(fx.paths, k, QStringLiteral("s"), kNow(),
                                           false, seq({1u}))),
                  QStringLiteral("bad_args")) << k.toStdString();
    }
    EXPECT_EQ(code(RunTraceVerb::start(fx.paths, QStringLiteral("G"), QString(),
                                       kNow(), false, seq({1u}))),
              QStringLiteral("bad_args"));
    EXPECT_EQ(code(RunTraceVerb::start(fx.paths, QStringLiteral("G"),
                                       QStringLiteral("a\nb"), kNow(), false,
                                       seq({1u}))),
              QStringLiteral("bad_args"));
}

// INV-2
TEST(RunTrace, StartRedrawsOnCollision) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    ASSERT_TRUE(writeAll(fx.index(),
        QStringLiteral("Genre: record\n\n") + header()
        + QStringLiteral("\n|---|---|---|---|---|\n"
                         "| G-20260924-a3f1 | 2026-09-24 | x | 1 | done |\n")));
    const QJsonObject r = RunTraceVerb::start(fx.paths, QStringLiteral("G"),
                                              QStringLiteral("x"), kNow(), false,
                                              seq({0xa3f1u, 0x0042u}));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("id")).toString(),
              QStringLiteral("G-20260924-0042"));
}

// INV-3
TEST(RunTrace, FinishRequiresLanes) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx);
    ASSERT_FALSE(id.isEmpty());

    QJsonObject a = basicArgs();
    a.remove(QStringLiteral("lanes"));
    EXPECT_EQ(code(finishWith(fx, id, a)), QStringLiteral("missing_field"));
    a[QStringLiteral("lanes")] = -1;
    EXPECT_EQ(code(finishWith(fx, id, a)), QStringLiteral("bad_args"));
    a[QStringLiteral("lanes")] = 1.5;
    EXPECT_EQ(code(finishWith(fx, id, a)), QStringLiteral("bad_args"));
    a[QStringLiteral("lanes")] = QStringLiteral("3");
    EXPECT_EQ(code(finishWith(fx, id, a)), QStringLiteral("bad_args"));
    EXPECT_FALSE(QFile::exists(fx.index()));

    const QJsonObject ok = finishWith(fx, id, basicArgs(0));
    ASSERT_TRUE(ok.value(QStringLiteral("ok")).toBool()) << code(ok).toStdString();
    EXPECT_TRUE(readAll(fx.index()).contains(
        QStringLiteral("| %1 | 2026-09-24 | docs/specs/x.md | 0 | ").arg(id)));
}

// INV-4
TEST(RunTrace, FinishCreatesIndexWithHeader) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx);
    const QJsonObject r = finishWith(fx, id, basicArgs(3));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << code(r).toStdString();
    const QString text = readAll(fx.index());
    EXPECT_TRUE(text.startsWith(QStringLiteral("Genre: record\n")));
    EXPECT_TRUE(text.contains(header() + QLatin1Char('\n')));
    const QString row = QStringLiteral(
        "| %1 | 2026-09-24 | docs/specs/x.md | 3 | complete, 2 findings fixed |").arg(id);
    EXPECT_TRUE(text.contains(row + QLatin1Char('\n'))) << text.toStdString();
    EXPECT_EQ(r.value(QStringLiteral("row")).toString(), row);

    // A second run appends below the first, inside the table.
    const QJsonObject r2 = RunTraceVerb::start(fx.paths, QStringLiteral("V"),
                                               QStringLiteral("feature"), kNow(),
                                               false, seq({0x0001u}));
    finishWith(fx, r2.value(QStringLiteral("id")).toString(), basicArgs(1));
    const QStringList lines = readAll(fx.index()).split(QLatin1Char('\n'));
    const int a = lines.indexOf(row);
    ASSERT_GE(a, 0);
    EXPECT_TRUE(lines.value(a + 1).startsWith(QStringLiteral("| V-20260924-0001 |")));
}

// INV-5
TEST(RunTrace, ForeignIndexIsRefused) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    ASSERT_TRUE(writeAll(fx.index(), QStringLiteral(
        "# Log\n\n| id | when |\n|---|---|\n| G-1 | today |\n")));
    const QString id = startId(fx);
    const auto before = snapshot(fx.paths.root);
    const QJsonObject r = finishWith(fx, id, basicArgs());
    EXPECT_EQ(code(r), QStringLiteral("format_mismatch"));
    EXPECT_EQ(snapshot(fx.paths.root), before);
}

// INV-6
TEST(RunTrace, CellsCannotBreakTheRow) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx, QStringLiteral("G"), QStringLiteral("a|b.md"));
    QJsonObject a = basicArgs();
    a[QStringLiteral("outcome")] = QStringLiteral("one\ntwo");
    EXPECT_EQ(code(finishWith(fx, id, a)), QStringLiteral("bad_args"));
    a[QStringLiteral("outcome")] = QStringLiteral("x | y");
    ASSERT_TRUE(finishWith(fx, id, a).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(readAll(fx.index()).contains(
        QStringLiteral("| %1 | 2026-09-24 | a\\|b.md | 3 | x \\| y |").arg(id)));
}

// INV-7
TEST(RunTrace, DetailFileBesideTheRow) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx);
    ASSERT_TRUE(finishWith(fx, id, basicArgs()).value(QStringLiteral("ok")).toBool());
    const QString detail = readAll(fx.at(QStringLiteral("docs/reviews/%1.md").arg(id)));
    EXPECT_TRUE(detail.startsWith(QStringLiteral("Genre: record\n")));
    EXPECT_TRUE(detail.contains(QStringLiteral("Lanes read it.")));

    // No `detail`, file already there → accepted untouched.
    const QJsonObject s2 = RunTraceVerb::start(fx.paths, QStringLiteral("E"),
                                               QStringLiteral("y"), kNow(), false,
                                               seq({0x0002u}));
    const QString id2 = s2.value(QStringLiteral("id")).toString();
    const QString own = QStringLiteral("Genre: record\n\nmine\n");
    ASSERT_TRUE(writeAll(fx.at(QStringLiteral("docs/reviews/%1.md").arg(id2)), own));
    QJsonObject a = basicArgs(0);
    a.remove(QStringLiteral("detail"));
    ASSERT_TRUE(finishWith(fx, id2, a).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readAll(fx.at(QStringLiteral("docs/reviews/%1.md").arg(id2))), own);

    // No `detail` and no file → missing_field.
    const QJsonObject s3 = RunTraceVerb::start(fx.paths, QStringLiteral("E"),
                                               QStringLiteral("z"), kNow(), false,
                                               seq({0x0003u}));
    EXPECT_EQ(code(finishWith(fx, s3.value(QStringLiteral("id")).toString(), a)),
              QStringLiteral("missing_field"));
}

// INV-8
TEST(RunTrace, CostNeedsCoverageAndCountsEachMessageOnce) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QJsonObject s = RunTraceVerb::start(fx.paths, QStringLiteral("G"),
                                              QStringLiteral("docs/specs/x.md"),
                                              kNow(), false, seq({0x0010u}));
    const QString id = s.value(QStringLiteral("id")).toString();
    const QString started = s.value(QStringLiteral("started_at")).toString();
    ASSERT_FALSE(started.isEmpty());

    // One turn before the start, one message split over two lines after it
    // (identical usage, as the harness writes them), and one more message.
    const auto line = [](const QString &ts, const QString &mid, int in, int cc,
                         int out, int cr) {
        QJsonObject u{{QStringLiteral("input_tokens"), in},
                      {QStringLiteral("cache_creation_input_tokens"), cc},
                      {QStringLiteral("output_tokens"), out},
                      {QStringLiteral("cache_read_input_tokens"), cr}};
        QJsonObject m{{QStringLiteral("id"), mid}, {QStringLiteral("usage"), u}};
        QJsonObject e{{QStringLiteral("type"), QStringLiteral("assistant")},
                      {QStringLiteral("timestamp"), ts},
                      {QStringLiteral("message"), m}};
        return QString::fromUtf8(QJsonDocument(e).toJson(QJsonDocument::Compact));
    };
    const QString before = utcStamp(kNow().addSecs(-60));
    const QString after = utcStamp(kNow().addSecs(60));
    const QString transcript = fx.dir.filePath(QStringLiteral("t.jsonl"));
    ASSERT_TRUE(writeAll(transcript,
        line(before, QStringLiteral("m0"), 1000, 1000, 1000, 1000) + '\n'
        + line(after, QStringLiteral("m1"), 10, 20, 30, 400) + '\n'
        + line(after, QStringLiteral("m1"), 10, 20, 30, 400) + '\n'
        + QStringLiteral("{\"type\":\"user\"}\n")
        + line(after, QStringLiteral("m2"), 1, 2, 3, 40) + '\n'));

    QJsonObject cost{{QStringLiteral("kind"), QStringLiteral("spec")},
                     {QStringLiteral("loop"), 1},
                     {QStringLiteral("dismissed"), 0},
                     {QStringLiteral("executed"), QStringLiteral("yes")},
                     {QStringLiteral("outcome"), QStringLiteral("complete")},
                     {QStringLiteral("arm"), QStringLiteral("draft")}};
    QJsonObject a = basicArgs(3);
    a[QStringLiteral("cost")] = cost;
    EXPECT_EQ(code(finishWith(fx, id, a, false, transcript)),
              QStringLiteral("coverage_required"));

    cost[QStringLiteral("verified")] = 2;
    a[QStringLiteral("cost")] = cost;
    a[QStringLiteral("lanes")] = 0;
    EXPECT_EQ(code(finishWith(fx, id, a, false, transcript)), QStringLiteral("bad_args"));
    a[QStringLiteral("lanes")] = 3;

    const QString tsv = fx.at(QStringLiteral("docs/reviews/run-costs.tsv"));
    ASSERT_TRUE(writeAll(tsv, QStringLiteral("date\tsubject\n")));
    const auto snap = snapshot(fx.paths.root);
    EXPECT_EQ(code(finishWith(fx, id, a, false, transcript)),
              QStringLiteral("format_mismatch"));
    EXPECT_EQ(snapshot(fx.paths.root), snap);
    QFile::remove(tsv);

    const QJsonObject r = finishWith(fx, id, a, false, transcript);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << code(r).toStdString()
        << r.value(QStringLiteral("error")).toString().toStdString();
    const QJsonObject c = r.value(QStringLiteral("cost_row")).toObject();
    EXPECT_EQ(c.value(QStringLiteral("new_tokens")).toInt(), 10 + 20 + 30 + 1 + 2 + 3);
    EXPECT_EQ(c.value(QStringLiteral("cache_reads")).toInt(), 400 + 40);
    EXPECT_EQ(c.value(QStringLiteral("messages_counted")).toInt(), 2);
    EXPECT_EQ(c.value(QStringLiteral("duplicate_lines_skipped")).toInt(), 1);
    const QStringList rows = readAll(tsv).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0], QStringLiteral("date\tarm\tsubject\tkind\tloop\tagents\t"
                                      "new_tokens\tcache_reads\tverified\t"
                                      "dismissed\texecuted\toutcome"));
    EXPECT_EQ(rows[1], QStringLiteral("2026-09-24\tdraft\tdocs/specs/x.md\tspec\t1\t3\t"
                                      "66\t440\t2\t0\tyes\tcomplete"));
}

// INV-9
TEST(RunTrace, OneRowPerId) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx);
    ASSERT_TRUE(finishWith(fx, id, basicArgs()).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(code(finishWith(fx, id, basicArgs())), QStringLiteral("already_recorded"));
    EXPECT_EQ(code(finishWith(fx, QStringLiteral("G-20260924-ffff"), basicArgs())),
              QStringLiteral("not_found"));
}

// INV-10
TEST(RunTrace, DryRunWritesNothing) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const auto rootBefore = snapshot(fx.paths.root);
    const auto stateBefore = snapshot(fx.paths.stateDir);
    const QJsonObject s = RunTraceVerb::start(fx.paths, QStringLiteral("G"),
                                              QStringLiteral("x"), kNow(), true,
                                              seq({0x0005u}));
    EXPECT_TRUE(s.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(s.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(snapshot(fx.paths.root), rootBefore);
    EXPECT_EQ(snapshot(fx.paths.stateDir), stateBefore);

    const QString id = startId(fx);
    const auto root2 = snapshot(fx.paths.root);
    const auto state2 = snapshot(fx.paths.stateDir);
    const QJsonObject f = finishWith(fx, id, basicArgs(), true);
    EXPECT_TRUE(f.value(QStringLiteral("ok")).toBool()) << code(f).toStdString();
    EXPECT_TRUE(f.value(QStringLiteral("dry_run")).toBool());
    EXPECT_FALSE(f.value(QStringLiteral("row")).toString().isEmpty());
    EXPECT_EQ(snapshot(fx.paths.root), root2);
    EXPECT_EQ(snapshot(fx.paths.stateDir), state2);
}

// INV-11
TEST(RunTrace, GetReportsThreeStates) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    const QString id = startId(fx);
    QJsonObject g = RunTraceVerb::get(fx.paths, id);
    EXPECT_TRUE(g.value(QStringLiteral("found")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("state")).toString(), QStringLiteral("pending"));
    EXPECT_EQ(g.value(QStringLiteral("subject")).toString(), QStringLiteral("docs/specs/x.md"));

    ASSERT_TRUE(finishWith(fx, id, basicArgs(2)).value(QStringLiteral("ok")).toBool());
    g = RunTraceVerb::get(fx.paths, id);
    EXPECT_EQ(g.value(QStringLiteral("state")).toString(), QStringLiteral("recorded"));
    const QJsonObject row = g.value(QStringLiteral("row")).toObject();
    EXPECT_EQ(row.value(QStringLiteral("run")).toString(), id);
    EXPECT_EQ(row.value(QStringLiteral("date")).toString(), QStringLiteral("2026-09-24"));
    EXPECT_EQ(row.value(QStringLiteral("lanes")).toString(), QStringLiteral("2"));
    EXPECT_EQ(row.value(QStringLiteral("outcome")).toString(),
              QStringLiteral("complete, 2 findings fixed"));

    g = RunTraceVerb::get(fx.paths, QStringLiteral("G-20260924-ffff"));
    EXPECT_TRUE(g.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(g.value(QStringLiteral("found")).toBool());
}

// INV-12
TEST(RunTrace, GateLogKeyIsHonoured) {
    Fixture fx;
    ASSERT_TRUE(fx.init());
    QString warn, err;
    EXPECT_EQ(RunTraceVerb::resolveIndex(fx.paths.root, &warn, &err),
              QStringLiteral("docs/reviews/gate-log.md"));
    EXPECT_TRUE(warn.isEmpty());

    ASSERT_TRUE(writeAll(fx.at(QStringLiteral(".claude/workflow.json")),
                         QStringLiteral("{\"gate_log\": \"records/trace.md\"}")));
    EXPECT_EQ(RunTraceVerb::resolveIndex(fx.paths.root, &warn, &err),
              QStringLiteral("records/trace.md"));

    ASSERT_TRUE(writeAll(fx.at(QStringLiteral(".claude/workflow.json")),
                         QStringLiteral("{not json")));
    warn.clear();
    EXPECT_EQ(RunTraceVerb::resolveIndex(fx.paths.root, &warn, &err),
              QStringLiteral("docs/reviews/gate-log.md"));
    EXPECT_FALSE(warn.isEmpty());

    ASSERT_TRUE(writeAll(fx.at(QStringLiteral(".claude/workflow.json")),
                         QStringLiteral("{\"gate_log\": \"../outside.md\"}")));
    err.clear();
    EXPECT_TRUE(RunTraceVerb::resolveIndex(fx.paths.root, &warn, &err).isEmpty());
    EXPECT_FALSE(err.isEmpty());

    // The root is the git top level, as gate-record finds it.
    ASSERT_TRUE(QDir().mkpath(fx.at(QStringLiteral(".git"))));
    ASSERT_TRUE(QDir().mkpath(fx.at(QStringLiteral("a/b"))));
    EXPECT_EQ(RunTraceVerb::projectRootFor(fx.at(QStringLiteral("a/b"))), fx.paths.root);
}

// INV-13
TEST(RunTrace, TranscriptIsNamedNeverGuessed) {
    EXPECT_EQ(RunTraceVerb::transcriptForSession(
                  QStringLiteral("/h/.claude/projects"),
                  QStringLiteral("/mnt/Games/Ants_Terminal"),
                  QStringLiteral("d6db5ea4-7c86-43a5-b5c9-77937072a3e9")),
              QStringLiteral("/h/.claude/projects/-mnt-Games-Ants-Terminal/"
                             "d6db5ea4-7c86-43a5-b5c9-77937072a3e9.jsonl"));
    EXPECT_TRUE(RunTraceVerb::transcriptForSession(
                    QStringLiteral("/h/.claude/projects"), QStringLiteral("/x"),
                    QStringLiteral("../../etc/passwd")).isEmpty());

    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString projects = d.filePath(QStringLiteral("projects"));
    const QString inside = projects + QStringLiteral("/p/s.jsonl");
    ASSERT_TRUE(writeAll(inside, QStringLiteral("{}\n")));
    const QString outside = d.filePath(QStringLiteral("s.jsonl"));
    ASSERT_TRUE(writeAll(outside, QStringLiteral("{}\n")));
    EXPECT_TRUE(RunTraceVerb::transcriptAllowed(inside, {projects}));
    EXPECT_FALSE(RunTraceVerb::transcriptAllowed(outside, {projects}));
    EXPECT_FALSE(RunTraceVerb::transcriptAllowed(projects + QStringLiteral("/p/../../s.jsonl"),
                                                 {projects}));

    // No code path guesses: the seam has no "newest transcript" helper at all.
    std::ifstream f(ANTS_SRC_DIR "/runtraceverb.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    ASSERT_FALSE(src.empty());
    EXPECT_EQ(src.find("st_mtime"), std::string::npos);
    EXPECT_EQ(src.find("QDir::Time"), std::string::npos);
}

// INV-14
TEST(RunTrace, RegisteredRequiredWithStrictSchema) {
    const auto slurp = [](const char *rel) {
        std::ifstream f(std::string(ANTS_SRC_DIR) + rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string mw = slurp("/mainwindow.cpp");
    const std::string ci = slurp("/claudeintegration.cpp");
    ASSERT_FALSE(mw.empty());
    ASSERT_FALSE(ci.empty());
    const auto reg = mw.find("registerToolProvider(\"run_trace\"");
    ASSERT_NE(reg, std::string::npos);
    EXPECT_NE(mw.substr(reg, 200).find("CallerCwdContract::Required"), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"run_trace\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"run_trace\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    EXPECT_NE(ci.substr(schema, end - schema).find("schema[\"additionalProperties\"] = false;"),
              std::string::npos);
}
